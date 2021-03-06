/*************************************************************************
 *  Copyright (C) 2008,2009 by Volker Lanz <vl@fidra.de>                 *
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

#include "fs/ext2.h"

#include "util/externalcommand.h"
#include "util/capacity.h"

#include <QRegularExpression>
#include <QString>

namespace FS
{
FileSystem::CommandSupportType ext2::m_GetUsed = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType ext2::m_GetLabel = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType ext2::m_Create = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType ext2::m_Grow = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType ext2::m_Shrink = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType ext2::m_Move = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType ext2::m_Check = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType ext2::m_Copy = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType ext2::m_Backup = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType ext2::m_SetLabel = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType ext2::m_UpdateUUID = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType ext2::m_GetUUID = FileSystem::cmdSupportNone;

ext2::ext2(qint64 firstsector, qint64 lastsector, qint64 sectorsused, const QString& label, FileSystem::Type t) :
    FileSystem(firstsector, lastsector, sectorsused, label, t)
{
}

void ext2::init()
{
    m_GetUsed = findExternal(QStringLiteral("dumpe2fs")) ? cmdSupportFileSystem : cmdSupportNone;
    m_GetLabel = cmdSupportCore;
    m_SetLabel = findExternal(QStringLiteral("e2label")) ? cmdSupportFileSystem : cmdSupportNone;
    m_Create = findExternal(QStringLiteral("mkfs.ext2")) ? cmdSupportFileSystem : cmdSupportNone;
    m_Check = findExternal(QStringLiteral("e2fsck"), { QStringLiteral("-V") }) ? cmdSupportFileSystem : cmdSupportNone;
    m_UpdateUUID = findExternal(QStringLiteral("tune2fs")) ? cmdSupportFileSystem : cmdSupportNone;
    m_Grow = (m_Check != cmdSupportNone && findExternal(QStringLiteral("resize2fs"))) ? cmdSupportFileSystem : cmdSupportNone;
    m_Shrink = (m_Grow != cmdSupportNone && m_GetUsed) != cmdSupportNone ? cmdSupportFileSystem : cmdSupportNone;
    m_Copy = (m_Check != cmdSupportNone) ? cmdSupportCore : cmdSupportNone;
    m_Move = (m_Check != cmdSupportNone) ? cmdSupportCore : cmdSupportNone;
    m_Backup = cmdSupportCore;
    m_GetUUID = cmdSupportCore;
}

bool ext2::supportToolFound() const
{
    return
        m_GetUsed != cmdSupportNone &&
        m_GetLabel != cmdSupportNone &&
        m_SetLabel != cmdSupportNone &&
        m_Create != cmdSupportNone &&
        m_Check != cmdSupportNone &&
        m_UpdateUUID != cmdSupportNone &&
        m_Grow != cmdSupportNone &&
        m_Shrink != cmdSupportNone &&
        m_Copy != cmdSupportNone &&
        m_Move != cmdSupportNone &&
        m_Backup != cmdSupportNone &&
        m_GetUUID != cmdSupportNone;
}

FileSystem::SupportTool ext2::supportToolName() const
{
    return SupportTool(QStringLiteral("e2fsprogs"), QUrl(QStringLiteral("http://e2fsprogs.sf.net")));
}

qint64 ext2::maxCapacity() const
{
    return 16 * Capacity::unitFactor(Capacity::Byte, Capacity::TiB) - Capacity::unitFactor(Capacity::Byte, Capacity::MiB);
}

qint64 ext2::maxLabelLength() const
{
    return 16;
}

qint64 ext2::readUsedCapacity(const QString& deviceNode) const
{
    ExternalCommand cmd(QStringLiteral("dumpe2fs"), { QStringLiteral("-h"), deviceNode });

    if (cmd.run()) {
        qint64 blockCount = -1;
        QRegularExpression re(QStringLiteral("Block count:\\s+(\\d+)"));
        QRegularExpressionMatch reBlockCount = re.match(cmd.output());

        if (reBlockCount.hasMatch())
            blockCount = reBlockCount.captured(1).toLongLong();

        qint64 freeBlocks = -1;
        re.setPattern(QStringLiteral("Free blocks:\\s+(\\d+)"));
        QRegularExpressionMatch reFreeBlocks = re.match(cmd.output());

        if (reFreeBlocks.hasMatch())
            freeBlocks = reFreeBlocks.captured(1).toLongLong();

        qint64 blockSize = -1;
        re.setPattern(QStringLiteral("Block size:\\s+(\\d+)"));
        QRegularExpressionMatch reBlockSize = re.match(cmd.output());

        if (reBlockSize.hasMatch())
            blockSize = reBlockSize.captured(1).toLongLong();

        if (blockCount > -1 && freeBlocks > -1 && blockSize > -1)
            return (blockCount - freeBlocks) * blockSize;
    }

    return -1;
}

bool ext2::check(Report& report, const QString& deviceNode) const
{
    ExternalCommand cmd(report, QStringLiteral("e2fsck"), { QStringLiteral("-f"), QStringLiteral("-y"), QStringLiteral("-v"), deviceNode });
    return cmd.run(-1) && (cmd.exitCode() == 0 || cmd.exitCode() == 1 || cmd.exitCode() == 2 || cmd.exitCode() == 256);
}

bool ext2::create(Report& report, const QString& deviceNode)
{
    ExternalCommand cmd(report, QStringLiteral("mkfs.ext2"), { QStringLiteral("-qF"), deviceNode });
    return cmd.run(-1) && cmd.exitCode() == 0;
}

bool ext2::resize(Report& report, const QString& deviceNode, qint64 length) const
{
    const QString len = QString::number(length / 512) + QStringLiteral("s");

    ExternalCommand cmd(report, QStringLiteral("resize2fs"), { deviceNode, len });
    return cmd.run(-1) && cmd.exitCode() == 0;
}

bool ext2::writeLabel(Report& report, const QString& deviceNode, const QString& newLabel)
{
    ExternalCommand cmd(report, QStringLiteral("e2label"), { deviceNode, newLabel });
    return cmd.run(-1) && cmd.exitCode() == 0;
}

bool ext2::writeLabelOnline(Report& report, const QString& deviceNode, const QString& mountPoint, const QString& newLabel)
{
    Q_UNUSED(mountPoint)
    return writeLabel(report, deviceNode, newLabel);
}

bool ext2::updateUUID(Report& report, const QString& deviceNode) const
{
    ExternalCommand cmd(report, QStringLiteral("tune2fs"), { QStringLiteral("-U"), QStringLiteral("random"), deviceNode });
    return cmd.run(-1) && cmd.exitCode() == 0;
}
}
