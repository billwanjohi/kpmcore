/*************************************************************************
 *  Copyright (C) 2016 by Chantara Tith <tith.chantara@gmail.com>        *
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

#include "jobs/movephysicalvolumejob.h"

#include "core/lvmdevice.h"

#include "util/report.h"

#include <KLocalizedString>

/** Creates a new MovePhysicalVolumeJob
    @param vgname
    @parem pvList
*/
MovePhysicalVolumeJob::MovePhysicalVolumeJob(LvmDevice& dev, const QStringList partlist) :
    Job(),
    m_Device(dev),
    m_PartList(partlist)
{
}

bool MovePhysicalVolumeJob::run(Report& parent)
{
    bool rval = false;

    Report* report = jobStarted(parent);

    //TODO:check that the provided list is legal

    QStringList destinations = LvmDevice::getPVs(device().name());
    foreach (QString partPath, partList()) {
        if (destinations.contains(partPath)) {
            destinations.removeAll(partPath);
        }
    }

    foreach (QString partPath, partList()) {
        rval = LvmDevice::movePV(*report, device(), partPath, destinations);
        if (rval == false) {
            break;
        }
    }

    jobFinished(*report, rval);

    return rval;
}

QString MovePhysicalVolumeJob::description() const
{
    QString tmp = QString();
    foreach (QString path, partList()) {
        tmp += path + QStringLiteral(",");
    }
    return xi18nc("@info/plain", "Move used PE in %1 on %2 to other available Physical Volumes", tmp, device().name());
}