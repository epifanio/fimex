/*
 * Fimex, ReferenceTimeSliceBuilder.cc
 *
 * (C) Copyright 2011, met.no
 *
 * Project Info:  https://wiki.met.no/fimex/start
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 *
 *  Created on: Jun 1, 2011
 *      Author: Heiko Klein
 */

#include "fimex/ReferenceTimeSliceBuilder.h"
#include "fimex/coordSys/CoordinateSystem.h"
#include "fimex/CDMException.h"
#include "fimex/Logger.h"

namespace MetNoFimex
{

static LoggerPtr logger = getLogger("fimex.ReferenceTimeSliceBuilder");

using namespace std;

ReferenceTimeSliceBuilder::ReferenceTimeSliceBuilder(const CDM& cdm, const std::string& varName, boost::shared_ptr<const CoordinateSystem> cs)
: SliceBuilder(cdm, varName), cs_(cs)
{
    if (! (cs->isComplete(varName) && cs->isCSFor(varName))) {
        throw CDMException("coordinate system not complete and for variable "+ varName + " as required for ReferenceTimeSliceBuilder");
    }
    setReferenceTimePos(0); // referenceTime is allways set
    if (cs_->hasAxisType(CoordinateAxis::Time)) {
        tShape_ = cs_->getTimeAxis()->getShape();
    }
}

void ReferenceTimeSliceBuilder::setReferenceTimePos(size_t refTimePos)
{
    if (cs_->hasAxisType(CoordinateAxis::ReferenceTime)) {
        CoordinateSystem::ConstAxisPtr refAxis = cs_->findAxisOfType(CoordinateAxis::ReferenceTime);
        if (refAxis->isExplicit()) {
            setStartAndSize(refAxis, refTimePos, 1);
        } else {
            LOG4FIMEX(logger, Logger::DEBUG, "referenceTime " << refAxis->getName() << " not explicit, ignoring position");
        }
    }
}
void ReferenceTimeSliceBuilder::setTimeStartAndSize(size_t start, size_t size)
{
    if (cs_->hasAxisType(CoordinateAxis::Time)) {
        CoordinateSystem::ConstAxisPtr tAxis = cs_->getTimeAxis();
        if (tAxis->isExplicit()) {
            setStartAndSize(tAxis, start, size);
        } else {
            set<string> tShape(tShape_.begin(), tShape_.end());
            // try to reduce the time-variables dimensions by extracting the refTime
            if (cs_->hasAxisType(CoordinateAxis::ReferenceTime)) {
                CoordinateSystem::ConstAxisPtr refAxis = cs_->findAxisOfType(CoordinateAxis::ReferenceTime);
                tShape.erase(refAxis->getName());
            }
            if (tShape.size() == 1) {
                setStartAndSize(*(tShape.begin()), start, size);
            } else {
                throw CDMException("unable to setTimeStartAndSize");
            }
        }
    } else {
        LOG4FIMEX(logger, Logger::DEBUG, "ReferenceTimeSliceBuilder without time, ignoring setTimeStartAndSize()");
    }
}
SliceBuilder ReferenceTimeSliceBuilder::getTimeVariableSliceBuilder()
{
    vector<string> names = tShape_;
    vector<size_t> dimSize(tShape_.size());
    const vector<size_t>& iMaxDimensions = getMaxDimensionSizes();
    for (size_t pos = 0; pos < names.size(); ++pos) {
        size_t iPos = getDimPos(names.at(pos));
        dimSize.at(pos) = iMaxDimensions.at(iPos);
    }
    SliceBuilder sb(names, dimSize);
    // now reduce the slicebuilder to the current start and size
    const vector<size_t>& iStart = getDimensionStartPositions();
    const vector<size_t>& iSize = getDimensionSizes();
    for (size_t pos = 0; pos < names.size(); ++pos) {
        size_t iPos = getDimPos(names.at(pos));
        sb.setStartAndSize(names.at(pos), iStart.at(iPos), iSize.at(iPos));
    }
    return sb;
}


}
