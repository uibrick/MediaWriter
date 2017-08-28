/*
 * Fedora Media Writer
 * Copyright (C) 2016 Martin Bříza <mbriza@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "blockdevice.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

#include <algorithm>
#include <array>
#include <utility>

#include <QDateTime>
#include <QtAlgorithms>
#include <QtEndian>
#include <QtGlobal>

#include "write.h"

BlockDevice::BlockDevice(int fd) : m_fd(fd), m_bytesWritten(0), m_totalBytes(0) {
    m_progress.fd = fd;
}

void BlockDevice::read() {
    seekEntry();
    for (std::size_t i = 0; i < MAX_PARTITIONS; ++i) {
        PartitionEntry entry;
        if (::read(m_fd, entry.data(), entry.size()) != entry.size()) {
            throw std::runtime_error("Failed to read partition table.");
        }
        const bool isEmtpy = std::all_of(entry.cbegin(), entry.cend(), [](const char c) { return c == '\0'; });
        if (isEmtpy) {
            continue;
        }
        m_entries.append(std::move(entry));
    }
}

void BlockDevice::seekEntry(std::size_t index) {
    if (::lseek(m_fd, PARTITION_ENTRY_OFFSET + PARTITION_ENTRY_SIZE * index, SEEK_SET) < 0) {
        throw std::runtime_error("Failed to seek to partition table.");
    }
}

void BlockDevice::seekto(std::size_t position) {
    if (::lseek(m_fd, position, SEEK_SET) < 0) {
        throw std::runtime_error("Failed to seek on block device.");
    }
}

void BlockDevice::writeZeros(std::size_t size) {
    constexpr std::size_t CHUNK_SIZE = 512;
    constexpr char zeros[CHUNK_SIZE]{};
    for (std::size_t i = 0; i < size; i += CHUNK_SIZE) {
        const auto len = ::write(m_fd, zeros, std::min(CHUNK_SIZE, size - i));
        if (len < 0) {
            throw std::runtime_error("Failed to write zeros to block device.");
        }
        m_bytesWritten += len;
        ::onProgress(&m_progress, m_bytesWritten, m_totalBytes);
    }
}

void BlockDevice::fillChs(char *chs, quint64 position) {
    Q_UNUSED(position);
    /**
     * It's guessed that the calculates values are correct but chs might as
     * well set to unused like so:
     */
    /*
    chs[0] = 0xff;
    chs[1] = 0xff;
    chs[2] = 0xef;
    */
    const int head = (position / NUM_SECTORS) % NUM_HEADS;
    const int sector = (position % NUM_SECTORS) + 1;
    const int cylinder = position / (NUM_HEADS * NUM_SECTORS);
    chs[0] = head & 0xff;
    // ccssssss
    chs[1] = ((cylinder >> 2) & 0xc0) | (sector & 0x3f);
    chs[2] = cylinder & 0xff;
}

int BlockDevice::addPartition(quint64 offset, quint64 size) {
    const quint32 lba = offset / SECTOR_SIZE;
    const quint32 count = size / SECTOR_SIZE;
    PartitionEntry entry;
    entry[0] = 0x0;
    fillChs(entry.data() + 1, offset);
    entry[4] = 0xb; // fat32
    fillChs(entry.data() + 5, offset + size);
    quint32 tmp = qToLittleEndian(lba);
    std::memcpy(entry.data() + 8, &tmp, sizeof(tmp));
    tmp = qToLittleEndian(count);
    std::memcpy(entry.data() + 12, &tmp, sizeof(tmp));

    const int num = m_entries.size() + 1;
    if (num > MAX_PARTITIONS) {
        throw std::runtime_error("Partition table is full.");
    }
    m_entries.append(std::move(entry));
    seekEntry(num - 1);
    auto bytes = ::write(m_fd, entry.data(), entry.size());
    if (bytes != static_cast<decltype(bytes)>(entry.size())) {
        throw std::runtime_error("Failed to add partition.");
    }
    return num;
}

/**
 * Format a partition with FAT32 and add an OVERLAY.IMG file that's zeroed out.
 */
void BlockDevice::formatOverlayPartition(quint64 offset, quint64 size) {
    /**
     * Magic values were generated by mkfs.fat (dosfstools).
     */
    constexpr quint8 bootSign[] = { 0x55, 0xaa };
    constexpr quint8 infoSector[] = { 0x52, 0x52, 0x61, 0x41 };
    constexpr quint8 fat[] = {
        0xf8, 0xff, 0xff, 0x0f, 0xff, 0xff, 0xff, 0x0f, 0xf8, 0xff, 0xff, 0x0f
    };
    constexpr quint8 fatEof[] = { 0xff, 0xff, 0xff, 0x0f };
    constexpr std::size_t fsinfoOffset = 480;

    constexpr int RESERVED_SECTORS = 32;
    constexpr int NR_FATS = 2;
    constexpr quint64 FIRST_CLUSTER = 3;
    constexpr std::array<int, 4> ranges = { { 260, 1024 * 8, 1024 * 16, 1024 * 32 } };

    auto align = [](quint64 number, quint64 alignment) -> quint64 {
        return (number + alignment - 1) & ~(alignment - 1);
    };
    auto getbyte = [](int number, int i) -> quint8 {
        return (number >> (8 * i)) & 0xff;
    };
    auto divCeil = [](int a, int b) -> int {
        return (a + b - 1) / b;
    };

    // Calulate cluster size.
    int sizeMb = size / (1024 * 1024);
    auto found = qLowerBound(ranges.begin(), ranges.end(), sizeMb);
    int sectorsPerCluster = found == ranges.begin() ? 1 : (found - ranges.begin()) * 8;
    int clusterSize = SECTOR_SIZE * sectorsPerCluster;

    // Calculate length of fat cluster allocation table.
    quint64 numSectors = size / SECTOR_SIZE;
    quint64 fatdata = numSectors - RESERVED_SECTORS;
    quint64 clusters = (fatdata * SECTOR_SIZE + NR_FATS * 8) / (clusterSize + NR_FATS * 4);
    quint64 fatlength = align(divCeil((clusters + 2) * 4, SECTOR_SIZE), sectorsPerCluster);

    // Calculate values that are dependend of the overlay file size.
    quint64 headerSize = (RESERVED_SECTORS + fatlength * 2 + sectorsPerCluster) * SECTOR_SIZE;
    quint64 maxFileSize = std::min(size - headerSize, 0xffffffffULL);
    maxFileSize = align(maxFileSize - SECTOR_SIZE - 1, SECTOR_SIZE);
    quint64 fileZeros = std::min(maxFileSize, 64ULL * 1024ULL);
    quint64 nextFreeCluster = FIRST_CLUSTER + (maxFileSize / clusterSize) + 1;
    quint64 numFreeClusters = ((size - headerSize) / clusterSize) + 1;

    auto fc = [&numFreeClusters, &getbyte](int i) {
        return getbyte(numFreeClusters, i);
    };
    auto nc = [&nextFreeCluster, &getbyte](int i) {
        return getbyte(nextFreeCluster, i);
    };
    quint8 fsinfo[] = {
        // Info sector signature.
        0x72, 0x72, 0x41, 0x61,
        // Number of free clusters.
        fc(0), fc(1), fc(2), fc(3),
        // Next free cluster.
        nc(0), nc(1), nc(2), nc(3)
    };
    auto t = [&numSectors, &getbyte](int i) {
        return getbyte(numSectors, i);
    };
    auto f = [&fatlength, &getbyte](int i) {
        return getbyte(fatlength, i);
    };
    // Generate UUID from time. This is similar to how mkfs.fat does it.
    quint32 volumeId = QDateTime::currentMSecsSinceEpoch() & 0xffffffff;
    auto u = [&volumeId, &getbyte](int i) {
        return getbyte(volumeId, i);
    };
    quint8 clsz = sectorsPerCluster;
    /**
     * Contains information like the fat label and other fixed values generated
     * by mkfs.fat. Values that can change are described by variable names
     * above.
     */
    quint8 bootSector[] = { 0xeb, 0x58, 0x90, 0x6d, 0x6b, 0x66, 0x73,
        0x2e, 0x66, 0x61, 0x74, 0x00, 0x02, clsz, 0x20, 0x00, 0x02, 0x00, 0x00,
        0x00, 0x00, 0xf8, 0x00, 0x00, 0x3e, 0x00, 0xf7, 0x00, 0x00, 0x00, 0x00,
        0x00, t(0), t(1), t(2), t(3), f(0), f(1), f(2), f(3), 0x00, 0x00, 0x00,
        0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x29,
        u(0), u(1), u(2), u(3), 0x4f, 0x56, 0x45, 0x52, 0x4c, 0x41, 0x59, 0x20,
        0x20, 0x20, 0x20, 0x46, 0x41, 0x54, 0x33, 0x32, 0x20, 0x20, 0x20 };
    // Will display an error message in case someone tries to boot from this
    // partition.
    quint8 bootCode[] = { 0x0e, 0x1f, 0xbe, 0x77, 0x7c, 0xac, 0x22, 0xc0,
        0x74, 0x0b, 0x56, 0xb4, 0x0e, 0xbb, 0x07, 0x00, 0xcd, 0x10, 0x5e, 0xeb,
        0xf0, 0x32, 0xe4, 0xcd, 0x16, 0xcd, 0x19, 0xeb, 0xfe, 0x54, 0x68, 0x69,
        0x73, 0x20, 0x69, 0x73, 0x20, 0x6e, 0x6f, 0x74, 0x20, 0x61, 0x20, 0x62,
        0x6f, 0x6f, 0x74, 0x61, 0x62, 0x6c, 0x65, 0x20, 0x64, 0x69, 0x73, 0x6b,
        0x2e, 0x20, 0x20, 0x50, 0x6c, 0x65, 0x61, 0x73, 0x65, 0x20, 0x69, 0x6e,
        0x73, 0x65, 0x72, 0x74, 0x20, 0x61, 0x20, 0x62, 0x6f, 0x6f, 0x74, 0x61,
        0x62, 0x6c, 0x65, 0x20, 0x66, 0x6c, 0x6f, 0x70, 0x70, 0x79, 0x20, 0x61,
        0x6e, 0x64, 0x0d, 0x0a, 0x70, 0x72, 0x65, 0x73, 0x73, 0x20, 0x61, 0x6e,
        0x79, 0x20, 0x6b, 0x65, 0x79, 0x20, 0x74, 0x6f, 0x20, 0x74, 0x72, 0x79,
        0x20, 0x61, 0x67, 0x61, 0x69, 0x6e, 0x20, 0x2e, 0x2e, 0x2e, 0x20, 0x0d,
        0x0a };
    constexpr std::size_t bootCodeSize = 420;
    auto datetime = QDateTime::currentDateTimeUtc();
    auto timeOnly = datetime.time();
    auto dateOnly = datetime.date();
    // Generate date and time according to the specification.
    quint64 time = ((timeOnly.second() + 1) >> 1) + (timeOnly.minute() << 5) + (timeOnly.hour() << 11);
    quint64 date = dateOnly.day() + (dateOnly.month() << 5) + (dateOnly.year() - 1980);
    quint8 tlo = time & 0xff;
    quint8 thi = time >> 8;
    quint8 dlo = date & 0xff;
    quint8 dhi = date >> 8;
    // Root directory will not be visible to user and contains stuff like the
    // fat label again. Apart from that it's like any other extent.
    quint8 rootDir[] = { 0x4f, 0x56, 0x45, 0x52, 0x4c, 0x41, 0x59, 0x20,
        0x20, 0x20, 0x20, 0x08, 0x00, 0x00, tlo, thi, dlo, dhi, dlo, dhi, 0x00,
        0x00, tlo, thi, dlo, dhi, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    // OVERLAY.IMG extent.
    auto s = [&maxFileSize, &getbyte](int i) {
        return getbyte(maxFileSize, i);
    };
    quint8 fileEntry[] = { 0x4f, 0x56, 0x45, 0x52, 0x4c, 0x41, 0x59, 0x20,
        0x49, 0x4d, 0x47, 0x20, 0x00, 0x00, tlo, thi, dlo, dhi, dlo, dhi, 0x00,
        0x00, tlo, thi, dlo, dhi, FIRST_CLUSTER, 0x00, s(0), s(1), s(2), s(3) };
    // Actually start writing stuff and keep track of progress.
    seekto(offset);
    m_bytesWritten = 0;
    m_totalBytes = headerSize + fileZeros;

    writeBytes<decltype(bootSector)>(bootSector);
    writeBytes<decltype(bootCode)>(bootCode);
    writeZeros(bootCodeSize - std::extent<decltype(bootCode)>::value);
    writeBytes<decltype(bootSign)>(bootSign);

    writeBytes<decltype(infoSector)>(infoSector);
    writeZeros(fsinfoOffset);
    writeBytes<decltype(fsinfo)>(fsinfo);
    writeZeros(14);
    writeBytes<decltype(bootSign)>(bootSign);

    writeZeros(SECTOR_SIZE * 4);

    // Backup boot sector because this is intended for flash memory.
    writeBytes<decltype(bootSector)>(bootSector);
    writeBytes<decltype(bootCode)>(bootCode);
    writeZeros(bootCodeSize - std::extent<decltype(bootCode)>::value);
    writeBytes<decltype(bootSign)>(bootSign);

    writeZeros(SECTOR_SIZE * 25);
    std::size_t used = std::extent<decltype(fat)>::value;
    used += (nextFreeCluster - (FIRST_CLUSTER + 1)) * 4;
    used += std::extent<decltype(fatEof)>::value;
    /**
     * Write cluster chain which marks which clusters have been used and which
     * are still available.
     */
    for (std::size_t i = 0; i < NR_FATS; ++i) {
        writeBytes<decltype(fat)>(fat);
        // Cluster number count starts with one because zero would mark a free cluster.
        for (quint64 cluster = FIRST_CLUSTER + 1; cluster < nextFreeCluster; ++cluster) {
            quint32 number = qToLittleEndian(cluster);
            writeBytes(&number);
        }
        writeBytes<decltype(fatEof)>(fatEof);
        writeZeros(SECTOR_SIZE * fatlength - used);
    }
    // Write extents.
    writeBytes<decltype(rootDir)>(rootDir);
    writeBytes<decltype(fileEntry)>(fileEntry);
    writeZeros(clusterSize - std::extent<decltype(rootDir)>::value - std::extent<decltype(fileEntry)>::value);
    writeZeros(fileZeros);
}