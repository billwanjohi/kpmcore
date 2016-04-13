/*************************************************************************
 *  Copyright (C) 2008-2012 by Volker Lanz <vl@fidra.de>                 *
 *  Copyright (C) 2015-2016 by Teo Mrnjavac <teo@kde.org>                *
 *  Copyright (C) 2016 by Andrius Štikonas <andrius@stikonas.eu>         *
 *                                                                       *
 *  This program is free software; you can redistribute it and/or        *
 *  modify it under the terms of the GNU General Public License as       *
 *  published by the Free Software Foundation; either version 3 of       *
 *  the License, or (at your option) any later version.                  *
 *                                                                       *
 *  This program is distributed in the hope that it will be useful,      *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 *  GNU General Public License for more details.                         *
 *                                                                       *
 *  You should have received a copy of the GNU General Public License    *
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 *************************************************************************/

/** @file
*/

#include "plugins/libparted/libpartedbackend.h"
#include "plugins/libparted/libparteddevice.h"

#include "core/device.h"
#include "core/partition.h"
#include "core/partitiontable.h"
#include "core/partitionalignment.h"

#include "fs/filesystem.h"
#include "fs/filesystemfactory.h"

#include "fs/fat16.h"
#include "fs/hfs.h"
#include "fs/hfsplus.h"
#include "fs/luks.h"

#include "util/globallog.h"
#include "util/helpers.h"

#include <QString>
#include <QStringList>
#include <QDebug>

#include <KLocalizedString>
#include <KIOCore/KMountPoint>
#include <KIOCore/KDiskFreeSpaceInfo>
#include <KPluginFactory>

#include <parted/parted.h>
#include <unistd.h>

K_PLUGIN_FACTORY_WITH_JSON(LibPartedBackendFactory, "pmlibpartedbackendplugin.json", registerPlugin<LibPartedBackend>();)

static struct {
    PedPartitionFlag pedFlag;
    PartitionTable::Flag flag;
} flagmap[] = {
    { PED_PARTITION_BOOT,               PartitionTable::FlagBoot },
    { PED_PARTITION_ROOT,               PartitionTable::FlagRoot },
    { PED_PARTITION_SWAP,               PartitionTable::FlagSwap },
    { PED_PARTITION_HIDDEN,             PartitionTable::FlagHidden },
    { PED_PARTITION_RAID,               PartitionTable::FlagRaid },
    { PED_PARTITION_LVM,                PartitionTable::FlagLvm },
    { PED_PARTITION_LBA,                PartitionTable::FlagLba },
    { PED_PARTITION_HPSERVICE,          PartitionTable::FlagHpService },
    { PED_PARTITION_PALO,               PartitionTable::FlagPalo },
    { PED_PARTITION_PREP,               PartitionTable::FlagPrep },
    { PED_PARTITION_MSFT_RESERVED,      PartitionTable::FlagMsftReserved },
    { PED_PARTITION_BIOS_GRUB,          PartitionTable::FlagBiosGrub },
    { PED_PARTITION_APPLE_TV_RECOVERY,  PartitionTable::FlagAppleTvRecovery },
    { PED_PARTITION_DIAG,               PartitionTable::FlagDiag }, // generic diagnostics flag
    { PED_PARTITION_LEGACY_BOOT,        PartitionTable::FlagLegacyBoot },
    { PED_PARTITION_MSFT_DATA,          PartitionTable::FlagMsftData },
    { PED_PARTITION_IRST,               PartitionTable::FlagIrst }, // Intel Rapid Start partition
    { PED_PARTITION_ESP,                PartitionTable::FlagEsp }   // EFI system
};

static QString s_lastPartedExceptionMessage;

/** Callback to handle exceptions from libparted
    @param e the libparted exception to handle
*/
static PedExceptionOption pedExceptionHandler(PedException* e)
{
    Log(Log::error) << i18nc("@info/plain", "LibParted Exception: %1", QString::fromLocal8Bit(e->message));
    s_lastPartedExceptionMessage = QString::fromLocal8Bit(e->message);
    return PED_EXCEPTION_UNHANDLED;
}

// --------------------------------------------------------------------------

// The following structs and the typedef come from libparted's internal gpt sources.
// It's very unfortunate there is no public API to get at the first and last usable
// sector for GPT a partition table, so this is the only (libparted) way to get that
// information (another way would be to read the GPT header and parse the
// information ourselves; if the libparted devs begin changing these internal
// structs for each point release and break our code, we'll have to do just that).

typedef struct {
    uint32_t time_low;
    uint16_t time_mid;
    uint16_t time_hi_and_version;
    uint8_t  clock_seq_hi_and_reserved;
    uint8_t  clock_seq_low;
    uint8_t  node[6];
} /* __attribute__ ((packed)) */ efi_guid_t;


struct __attribute__((packed)) _GPTDiskData {
    PedGeometry data_area;
    int     entry_count;
    efi_guid_t  uuid;
};

typedef struct _GPTDiskData GPTDiskData;

// --------------------------------------------------------------------------

/** Get the first sector a Partition may cover on a given Device
    @param d the Device in question
    @return the first sector usable by a Partition
*/
static quint64 firstUsableSector(const Device& d)
{
    PedDevice* pedDevice = ped_device_get(d.deviceNode().toLatin1().constData());
    PedDisk* pedDisk = pedDevice ? ped_disk_new(pedDevice) : nullptr;

    quint64 rval = 0;
    if (pedDisk)
        rval = pedDisk->dev->bios_geom.sectors;

    if (pedDisk && strcmp(pedDisk->type->name, "gpt") == 0) {
        GPTDiskData* gpt_disk_data = reinterpret_cast<GPTDiskData*>(pedDisk->disk_specific);
        PedGeometry* geom = reinterpret_cast<PedGeometry*>(&gpt_disk_data->data_area);

        if (geom)
            rval = geom->start;
        else
            rval += 32;
    }

    return rval;
}

/** Get the last sector a Partition may cover on a given Device
    @param d the Device in question
    @return the last sector usable by a Partition
*/
static quint64 lastUsableSector(const Device& d)
{
    PedDevice* pedDevice = ped_device_get(d.deviceNode().toLatin1().constData());
    PedDisk* pedDisk = pedDevice ? ped_disk_new(pedDevice) : nullptr;

    quint64 rval = 0;
    if (pedDisk)
        rval = static_cast< quint64 >( pedDisk->dev->bios_geom.sectors ) *
               pedDisk->dev->bios_geom.heads *
               pedDisk->dev->bios_geom.cylinders - 1;

    if (pedDisk && strcmp(pedDisk->type->name, "gpt") == 0) {
        GPTDiskData* gpt_disk_data = reinterpret_cast<GPTDiskData*>(pedDisk->disk_specific);
        PedGeometry* geom = reinterpret_cast<PedGeometry*>(&gpt_disk_data->data_area);

        if (geom)
            rval = geom->end;
        else
            rval -= 32;
    }

    return rval;
}

/** Reads sectors used on a FileSystem using libparted functions.
    @param pedDisk pointer to pedDisk  where the Partition and its FileSystem are
    @param p the Partition the FileSystem is on
    @return the number of sectors used
*/
#if defined LIBPARTED_FS_RESIZE_LIBRARY_SUPPORT
static qint64 readSectorsUsedLibParted(PedDisk* pedDisk, const Partition& p)
{
    Q_ASSERT(pedDisk);

    qint64 rval = -1;

    PedPartition* pedPartition = ped_disk_get_partition_by_sector(pedDisk, p.firstSector());

    if (pedPartition) {
        PedFileSystem* pedFileSystem = ped_file_system_open(&pedPartition->geom);

        if (pedFileSystem) {
            if (PedConstraint* pedConstraint = ped_file_system_get_resize_constraint(pedFileSystem)) {
                rval = pedConstraint->min_size;
                ped_constraint_destroy(pedConstraint);
            }

            ped_file_system_close(pedFileSystem);
        }
    }

    return rval;
}
#endif

/** Reads the sectors used in a FileSystem and stores the result in the Partition's FileSystem object.
    @param pedDisk pointer to pedDisk  where the Partition and its FileSystem are
    @param p the Partition the FileSystem is on
    @param mountPoint mount point of the partition in question
*/
static void readSectorsUsed(PedDisk* pedDisk, const Device& d, Partition& p, const QString& mountPoint)
{
    Q_ASSERT(pedDisk);

    const KDiskFreeSpaceInfo freeSpaceInfo = KDiskFreeSpaceInfo::freeSpaceInfo(mountPoint);

    if (p.isMounted() && freeSpaceInfo.isValid() && mountPoint != QStringLiteral())
        p.fileSystem().setSectorsUsed(freeSpaceInfo.used() / d.logicalSectorSize());
    else if (p.fileSystem().supportGetUsed() == FileSystem::cmdSupportFileSystem)
        p.fileSystem().setSectorsUsed(p.fileSystem().readUsedCapacity(p.deviceNode()) / d.logicalSectorSize());
#if defined LIBPARTED_FS_RESIZE_LIBRARY_SUPPORT
    else if (p.fileSystem().supportGetUsed() == FileSystem::cmdSupportCore)
        p.fileSystem().setSectorsUsed(readSectorsUsedLibParted(pedDisk, p));
#else
    Q_UNUSED(pedDisk);
#endif
}

static PartitionTable::Flags activeFlags(PedPartition* p)
{
    PartitionTable::Flags flags = PartitionTable::FlagNone;

    // We might get here with a pedPartition just picked up from libparted that is
    // unallocated. Libparted doesn't like it if we ask for flags for unallocated
    // space.
    if (p->num <= 0)
        return flags;

    for (quint32 i = 0; i < sizeof(flagmap) / sizeof(flagmap[0]); i++)
        if (ped_partition_is_flag_available(p, flagmap[i].pedFlag) && ped_partition_get_flag(p, flagmap[i].pedFlag))
            flags |= flagmap[i].flag;

    return flags;
}

static PartitionTable::Flags availableFlags(PedPartition* p)
{
    PartitionTable::Flags flags;

    // see above.
    if (p->num <= 0)
        return flags;

    for (quint32 i = 0; i < sizeof(flagmap) / sizeof(flagmap[0]); i++)
        if (ped_partition_is_flag_available(p, flagmap[i].pedFlag)) {
            // Workaround: libparted claims the hidden flag is available for extended partitions, but
            // throws an error when we try to set or clear it. So skip this combination. Also see setFlag.
            if (p->type != PED_PARTITION_EXTENDED || flagmap[i].flag != PartitionTable::FlagHidden)
                flags |= flagmap[i].flag;
        }

    return flags;
}

/** Constructs a LibParted object. */
LibPartedBackend::LibPartedBackend(QObject*, const QList<QVariant>&) :
    CoreBackend()
{
    ped_exception_set_handler(pedExceptionHandler);
}

void LibPartedBackend::initFSSupport()
{
#if defined LIBPARTED_FS_RESIZE_LIBRARY_SUPPORT
    if (FS::fat16::m_Shrink == FileSystem::cmdSupportNone)
        FS::fat16::m_Shrink = FileSystem::cmdSupportBackend;

    if (FS::fat16::m_Grow == FileSystem::cmdSupportNone)
        FS::fat16::m_Grow = FileSystem::cmdSupportBackend;

    if (FS::hfs::m_Shrink == FileSystem::cmdSupportNone)
        FS::hfs::m_Shrink = FileSystem::cmdSupportBackend;

    if (FS::hfsplus::m_Shrink == FileSystem::cmdSupportNone)
        FS::hfsplus::m_Shrink = FileSystem::cmdSupportBackend;

    if (FS::hfs::m_GetUsed == FileSystem::cmdSupportNone)
        FS::hfs::m_GetUsed = FileSystem::cmdSupportBackend;

    if (FS::hfsplus::m_GetUsed == FileSystem::cmdSupportNone)
        FS::hfsplus::m_GetUsed = FileSystem::cmdSupportBackend;
#endif
}

/** Scans a Device for Partitions.

    This method  will scan a Device for all Partitions on it, detect the FileSystem for each Partition,
    try to determine the FileSystem usage, read the FileSystem label and store it all in newly created
    objects that are in the end added to the Device's PartitionTable.

    @param pedDevice libparted pointer to the device
    @param d Device
    @param pedDisk libparted pointer to the partition table
*/
void LibPartedBackend::scanDevicePartitions(PedDevice*, Device& d, PedDisk* pedDisk)
{
    Q_ASSERT(pedDisk);
    Q_ASSERT(d.partitionTable());

    PedPartition* pedPartition = nullptr;

    KMountPoint::List mountPoints = KMountPoint::currentMountPoints(KMountPoint::NeedRealDeviceName);
    mountPoints.append(KMountPoint::possibleMountPoints(KMountPoint::NeedRealDeviceName));

    QList<Partition*> partitions;

    while ((pedPartition = ped_disk_next_partition(pedDisk, pedPartition))) {
        if (pedPartition->num < 1)
            continue;

        PartitionRole::Roles r = PartitionRole::None;
        FileSystem::Type type = detectFileSystem(pedPartition);

        switch (pedPartition->type) {
        case PED_PARTITION_NORMAL:
            r = PartitionRole::Primary;
            break;

        case PED_PARTITION_EXTENDED:
            r = PartitionRole::Extended;
            type = FileSystem::Extended;
            break;

        case PED_PARTITION_LOGICAL:
            r = PartitionRole::Logical;
            break;

        default:
            continue;
        }

        // Find an extended partition this partition is in.
        PartitionNode* parent = d.partitionTable()->findPartitionBySector(pedPartition->geom.start, PartitionRole(PartitionRole::Extended));

        // None found, so it's a primary in the device's partition table.
        if (parent == nullptr)
            parent = d.partitionTable();

        const QString node = QString::fromUtf8(ped_partition_get_path(pedPartition));
        FileSystem* fs = FileSystemFactory::create(type, pedPartition->geom.start, pedPartition->geom.end);

        // libparted does not handle LUKS partitions
        QString mountPoint;
        bool mounted = false;
        if (fs->type() == FileSystem::Luks) {
            FS::luks* luksFs = dynamic_cast<FS::luks*>(fs);
            QString mapperNode = FS::luks::mapperName(node);
            bool isCryptOpen = !mapperNode.isEmpty();
            luksFs->setCryptOpen(isCryptOpen);

            if (isCryptOpen) {
                luksFs->loadInnerFilesystem(mapperNode);

                mountPoint = mountPoints.findByDevice(mapperNode) ?
                             mountPoints.findByDevice(mapperNode)->mountPoint() :
                             QString();
                // We cannot use libparted to check the mounted status because
                // we don't have a PedPartition for the mapper device, so we use
                // check_mount_point from util-linux instead, defined in the
                // private header ismounted.h and copied into KPMcore & wrapped
                // in helpers.h for convenience.
                mounted = isMounted(mapperNode);
            } else {
                mounted = false;
            }

            luksFs->setMounted(mounted);
        } else {
            mountPoint = mountPoints.findByDevice(node) ?
                         mountPoints.findByDevice(node)->mountPoint() :
                         QString();
            mounted = ped_partition_is_busy(pedPartition);
        }

        Partition* part = new Partition(parent, d, PartitionRole(r), fs, pedPartition->geom.start, pedPartition->geom.end, node, availableFlags(pedPartition), mountPoint, mounted, activeFlags(pedPartition));

        readSectorsUsed(pedDisk, d, *part, mountPoint);

        if (fs->supportGetLabel() != FileSystem::cmdSupportNone)
            fs->setLabel(fs->readLabel(part->deviceNode()));

        if (fs->supportGetUUID() != FileSystem::cmdSupportNone)
            fs->setUUID(fs->readUUID(part->deviceNode()));

        parent->append(part);
        partitions.append(part);
    }

    d.partitionTable()->updateUnallocated(d);

    if (d.partitionTable()->isSectorBased(d))
        d.partitionTable()->setType(d, PartitionTable::msdos_sectorbased);

    foreach(const Partition * part, partitions)
    PartitionAlignment::isAligned(d, *part);

    ped_disk_destroy(pedDisk);
}

/** Create a Device for the given device_node and scan it for partitions.
    @param device_node the device node (e.g. "/dev/sda")
    @return the created Device object. callers need to free this.
*/
Device* LibPartedBackend::scanDevice(const QString& device_node)
{
    PedDevice* pedDevice = ped_device_get(device_node.toLocal8Bit().constData());

    if (pedDevice == nullptr) {
        Log(Log::warning) << xi18nc("@info/plain", "Could not access device <filename>%1</filename>", device_node);
        return nullptr;
    }

    Log(Log::information) << i18nc("@info/plain", "Device found: %1", QString::fromUtf8(pedDevice->model));

    Device* d = new Device(QString::fromUtf8(pedDevice->model), QString::fromUtf8(pedDevice->path), pedDevice->bios_geom.heads, pedDevice->bios_geom.sectors, pedDevice->bios_geom.cylinders, pedDevice->sector_size);

    PedDisk* pedDisk = ped_disk_new(pedDevice);

    if (pedDisk) {
        const PartitionTable::TableType type = PartitionTable::nameToTableType(QString::fromUtf8(pedDisk->type->name));
        CoreBackend::setPartitionTableForDevice(*d, new PartitionTable(type, firstUsableSector(*d), lastUsableSector(*d)));
        CoreBackend::setPartitionTableMaxPrimaries(*d->partitionTable(), ped_disk_get_max_primary_partition_count(pedDisk));

        scanDevicePartitions(pedDevice, *d, pedDisk);
    }

    return d;
}

QList<Device*> LibPartedBackend::scanDevices(bool excludeReadOnly)
{
    QList<Device*> result;

    ped_device_probe_all();
    PedDevice* pedDevice = nullptr;
    QVector<QString> path;
    quint32 totalDevices = 0;
    while (true) {
        pedDevice = ped_device_get_next(pedDevice);
        if (!pedDevice)
            break;
        if (pedDevice->type == PED_DEVICE_DM)
            continue;
        if (excludeReadOnly && (
                pedDevice->type == PED_DEVICE_LOOP ||
                pedDevice->read_only))
            continue;

        path.push_back(QString::fromUtf8(pedDevice->path));
        ++totalDevices;
    }
    for (quint32 i = 0; i < totalDevices; ++i) {
        emitScanProgress(path[i], i * 100 / totalDevices);
        Device* d = scanDevice(path[i]);
        if (d)
            result.append(d);
    }

    return result;
}

CoreBackendDevice* LibPartedBackend::openDevice(const QString& device_node)
{
    LibPartedDevice* device = new LibPartedDevice(device_node);

    if (device == nullptr || !device->open()) {
        delete device;
        device = nullptr;
    }

    return device;
}

CoreBackendDevice* LibPartedBackend::openDeviceExclusive(const QString& device_node)
{
    LibPartedDevice* device = new LibPartedDevice(device_node);

    if (device == nullptr || !device->openExclusive()) {
        delete device;
        device = nullptr;
    }

    return device;
}

bool LibPartedBackend::closeDevice(CoreBackendDevice* core_device)
{
    return core_device->close();
}

/** Detects the type of a FileSystem given a PedDevice and a PedPartition
    @param pedDevice pointer to the pedDevice. Must not be nullptr.
    @param pedPartition pointer to the pedPartition. Must not be nullptr
    @return the detected FileSystem type (FileSystem::Unknown if not detected)
*/
FileSystem::Type LibPartedBackend::detectFileSystem(PedPartition* pedPartition)
{
    FileSystem::Type rval = FileSystem::Unknown;

    char* pedPath = ped_partition_get_path(pedPartition);

    if (pedPath)
        rval = FileSystem::detectFileSystem(QString::fromUtf8(pedPath));

    free(pedPath);

    return rval;
}

PedPartitionFlag LibPartedBackend::getPedFlag(PartitionTable::Flag flag)
{
    for (quint32 i = 0; i < sizeof(flagmap) / sizeof(flagmap[0]); i++)
        if (flagmap[i].flag == flag)
            return flagmap[i].pedFlag;

    return static_cast<PedPartitionFlag>(-1);
}

QString LibPartedBackend::lastPartedExceptionMessage()
{
    return s_lastPartedExceptionMessage;
}

#include "libpartedbackend.moc"
