/*************************************************************************
 *  Copyright (C) 2008, 2010 by Volker Lanz <vl@fidra.de>                *
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

#include "ops/deleteoperation.h"

#include "core/partition.h"
#include "core/device.h"
#include "core/partitiontable.h"
#include "fs/luks.h"

#include "jobs/deletepartitionjob.h"
#include "jobs/deletefilesystemjob.h"
#include "jobs/shredfilesystemjob.h"

#include "util/capacity.h"

#include <QString>

#include <KLocalizedString>

/** Creates a new DeleteOperation
    @param d the Device to delete a Partition on
    @param p pointer to the Partition to delete. May not be nullptr
*/
DeleteOperation::DeleteOperation(Device& d, Partition* p, ShredAction shred) :
    Operation(),
    m_TargetDevice(d),
    m_DeletedPartition(p),
    m_ShredAction(shred),
    m_DeletePartitionJob(new DeletePartitionJob(targetDevice(), deletedPartition()))
{
    switch (shredAction()) {
    case NoShred:
        m_DeleteFileSystemJob = static_cast<Job*>(new DeleteFileSystemJob(targetDevice(), deletedPartition()));
        break;
    case ZeroShred:
        m_DeleteFileSystemJob = static_cast<Job*>(new ShredFileSystemJob(targetDevice(), deletedPartition(), false));
        break;
    case RandomShred:
        m_DeleteFileSystemJob = static_cast<Job*>(new ShredFileSystemJob(targetDevice(), deletedPartition(), true));
    }

    addJob(deleteFileSystemJob());
    addJob(deletePartitionJob());
}

DeleteOperation::~DeleteOperation()
{
    if (status() != StatusPending && status() != StatusNone) // don't delete the partition if we're being merged or undone
        delete m_DeletedPartition;
}

bool DeleteOperation::targets(const Device& d) const
{
    return d == targetDevice();
}

bool DeleteOperation::targets(const Partition& p) const
{
    return p == deletedPartition();
}

void DeleteOperation::preview()
{
    removePreviewPartition(targetDevice(), deletedPartition());
    checkAdjustLogicalNumbers(deletedPartition(), false);
}

void DeleteOperation::undo()
{
    checkAdjustLogicalNumbers(deletedPartition(), true);
    insertPreviewPartition(targetDevice(), deletedPartition());
}

QString DeleteOperation::description() const
{
    if (shredAction() != NoShred)
        return xi18nc("@info:status", "Shred partition <filename>%1</filename> (%2, %3)", deletedPartition().deviceNode(), Capacity::formatByteSize(deletedPartition().capacity()), deletedPartition().fileSystem().name());
    else
        return xi18nc("@info:status", "Delete partition <filename>%1</filename> (%2, %3)", deletedPartition().deviceNode(), Capacity::formatByteSize(deletedPartition().capacity()), deletedPartition().fileSystem().name());
}

void DeleteOperation::checkAdjustLogicalNumbers(Partition& p, bool undo)
{
    // If the deleted partition is a logical one, we need to adjust the numbers of the
    // other logical partitions in the extended one, if there are any, because the OS
    // will do that, too: Logicals must be numbered without gaps, i.e., a numbering like
    // sda5, sda6, sda8 (after sda7 is deleted) will become sda5, sda6, sda7
    Partition* parentPartition = dynamic_cast<Partition*>(p.parent());
    if (parentPartition && parentPartition->roles().has(PartitionRole::Extended))
        parentPartition->adjustLogicalNumbers(undo ? -1 : p.number(), undo ? p.number() : -1);
}

/** Can a Partition be deleted?
    @param p the Partition in question, may be nullptr.
    @return true if @p p can be deleted.
*/
bool DeleteOperation::canDelete(const Partition* p)
{
    if (p == nullptr)
        return false;

    if (p->isMounted())
        return false;

    if (p->roles().has(PartitionRole::Unallocated))
        return false;

    if (p->roles().has(PartitionRole::Extended))
        return p->children().size() == 1 && p->children()[0]->roles().has(PartitionRole::Unallocated);

    if (p->roles().has(PartitionRole::Luks))
    {
        const FS::luks* luksFs = dynamic_cast<const FS::luks*>(&p->fileSystem());
        if (!luksFs)
            return false;

        if (luksFs->isCryptOpen() || luksFs->isMounted())
            return false;
    }

    return true;
}
