/*
 * Fimex
 *
 * (C) Copyright 2008, met.no
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
 */

#include "fimex/Units.h"

#include "fimex/Logger.h"
#include "fimex/UnitsConverter.h"
#include "fimex/UnitsException.h"
#include "fimex/Utils.h"

#include <memory>

#include "MutexLock.h"
#include "fimex_config.h"
#ifdef HAVE_UDUNITS2_H
#include "udunits2.h"
#include "converter.h"
#else // UDUNITS1
extern "C" {
#include "udunits.h"
}
// add forgotten utIsInit fom udunits
extern "C" int utIsInit();
#endif // UDUNITS2

#include <cmath>

namespace MetNoFimex
{
#ifdef HAVE_UDUNITS2_H
static ut_system* utSystem;
#endif

static MutexType unitsMutex;
extern MutexType& getUnitsMutex() {
    return unitsMutex;
}

static Logger_p logger = getLogger("fimex.Units");

void handleUdUnitError(int unitErrCode, const std::string& message)
{
    switch (unitErrCode) {
#ifdef HAVE_UDUNITS2_H
    case UT_SUCCESS: break;
    case UT_BAD_ARG:  throw UnitException("An argument violates the function's contract: " + message);
    case UT_EXISTS: throw UnitException("Unit, prefix, or identifier already exists: " + message);
    case UT_NO_UNIT: throw UnitException("No such unit exists: " + message);
    case UT_OS: throw UnitException("Operating-system error: " + message);
    case UT_NOT_SAME_SYSTEM: throw UnitException("The units belong to different unit-systems: " + message);
    case UT_MEANINGLESS: throw UnitException("The operation on the unit(s) is meaningless: " + message);
    case UT_NO_SECOND: throw UnitException("The unit-system doesn't have a unit named 'second': " + message);
    case UT_VISIT_ERROR: throw UnitException("An error occurred while visiting a unit: " + message);
    case UT_CANT_FORMAT: throw UnitException("A unit can't be formatted in the desired manner: " + message);
    case UT_SYNTAX: throw UnitException("string unit representation contains syntax error: " + message);
    case UT_UNKNOWN: throw UnitException("string unit representation contains unknown word: " + message);
    case UT_OPEN_ARG: throw UnitException("Can't open argument-specified unit database: " + message);
    case UT_OPEN_ENV: throw UnitException("Can't open environment-specified unit database: " + message);
    case UT_OPEN_DEFAULT: throw UnitException("Can't open installed, default, unit database: " + message);
    case UT_PARSE: throw UnitException("Error parsing unit specification: " + message);
#else // udunits1
    case 0: break;
    case UT_EOF: throw UnitException("end-of-file encountered : " + message);
    case UT_ENOFILE: throw UnitException("no units-file : " + message);
    case UT_ESYNTAX: throw UnitException("syntax error : " + message);
    case UT_EUNKNOWN: throw UnitException("unknown specification : " + message);
    case UT_EIO: throw UnitException("I/O error : " + message);
    case UT_EINVALID: throw UnitException("invalid unit-structure : " + message);
    case UT_ENOINIT: throw UnitException("package not initialized : " + message);
    case UT_ECONVERT: throw UnitException("two units are not convertable : " + message);
    case UT_EALLOC: throw UnitException("memory allocation failure : " + message);
    case UT_ENOROOM: throw UnitException("insufficient room supplied : " + message);
    case UT_ENOTTIME: throw UnitException("not a unit of time : " + message);
    case UT_DUP: throw UnitException("duplicate unit : " + message);
#endif // UDUNITS2
    default: throw UnitException("unknown error");
    }
}

class LinearUnitsConverter : public UnitsConverter{
    double dscale_;
    double doffset_;
    double fscale_;
    double foffset_;

public:
    LinearUnitsConverter(double scale, double offset)
        : dscale_(scale)
        , doffset_(offset)
        , fscale_(scale)
        , foffset_(offset)
    {
    }
    ~LinearUnitsConverter() {}
    double convert(double from) override { return dscale_ * from + doffset_; }
    float convert(float from) override { return fscale_ * from + foffset_; }
    bool isLinear() override { return true; }
    void getScaleOffset(double& scale, double& offset) override
    {
        scale = dscale_;
        offset = doffset_;
    }
};

#ifdef HAVE_UDUNITS2_H
class Ud2UnitsConverter : public UnitsConverter {
    cv_converter* conv_;
public:
    Ud2UnitsConverter(cv_converter* conv) : conv_(conv) {}
    ~Ud2UnitsConverter() { cv_free(conv_); }
    double convert(double from) override
    {
        double retval;
#pragma omp critical (cv_converter)
        {
            retval = cv_convert_double(conv_, from);
        }
        return retval;
    }
    float convert(float from) override
    {
        float retval;
#pragma omp critical(cv_converter)
        {
            retval = cv_convert_float(conv_, from);
        }
        return retval;
    }
    bool isLinear() override
    {
        // check some points
        double offset = convert(0.0);
        if (!std::isfinite(offset)) return false;
        double slope = convert(1.0) - offset;
        if (!std::isfinite(slope)) return false;

        double val = 10.;
        double cval = convert(val);
        if ((!std::isfinite(cval)) || (std::fabs(cval - (val*slope+offset)) > 1e-5)) return false;
        val = 100.;
        cval = convert(val);
        if ((!std::isfinite(cval)) || (std::fabs(cval - (val*slope+offset)) > 1e-5)) return false;
        val = 1000.;
        cval = convert(val);
        if ((!std::isfinite(cval)) || (std::fabs(cval - (val*slope+offset)) > 1e-5)) return false;
        val = -1000.;
        cval = convert(val);
        if ((!std::isfinite(cval)) || (std::fabs(cval - (val*slope+offset)) > 1e-5)) return false;

        return true;
    }
    void getScaleOffset(double& scale, double& offset) override
    {
        if (! isLinear()) throw UnitException("cannot get scale and offset of non-linear function");
        offset = convert(0.0);
        scale = convert(1.) - offset;
        // make sure, scale is determined at a place where offset is no longer numerically superior
        if (scale != 0 && offset != 0) {
            double temp = -offset/scale;
            //std::cerr << temp << " offset:" << offset << " scale: " << scale <<  std::endl;
            double scale2 = (convert(temp)-offset)/temp;
            if (fabs(scale - scale2) < 1e-3) {
                // should only increase precision, not nan/inf or other cases
                scale = scale2;
            }
        }
    }
};
#endif

Units::Units()
{
    ScopedCritical lock(unitsMutex);
#ifdef HAVE_UDUNITS2_H
    if (utSystem == 0) {
        ut_set_error_message_handler(&ut_ignore);
        utSystem = ut_read_xml(0);
        handleUdUnitError(ut_get_status());
    }
#else
    if (!utIsInit()) {
        handleUdUnitError(utInit(0));
    }
#endif
}

Units::Units(const Units& u)
{}

Units& Units::operator=(const Units& rhs)
{
    return *this; // no state! no increase/decrease to counter required
}

Units::~Units()
{}

bool Units::unload(bool force)
{
    bool retVal = false;
    ScopedCritical lock(unitsMutex);
    if (force) {
#ifdef HAVE_UDUNITS2_H
        ut_free_system(utSystem);
#else
        utTerm();
#endif
        retVal = true;
    }

    return retVal;
}


void Units::convert(const std::string& from, const std::string& to, double& slope, double& offset)
{
    LOG4FIMEX(logger, Logger::DEBUG, "convert from '" << from << "' to '" << to << "'");
    UnitsConverter_p conv = getConverter(from, to);
    conv->getScaleOffset(slope, offset);
}

UnitsConverter_p Units::getConverter(const std::string& from, const std::string& to)
{
    LOG4FIMEX(logger, Logger::DEBUG, "getConverter from '" << from << "' to '" << to << "'");
    if (from == to) {
        return std::make_shared<LinearUnitsConverter>(1., 0.);
    }
    ScopedCritical lock(unitsMutex);
#ifdef HAVE_UDUNITS2_H
    std::shared_ptr<ut_unit> fromUnit(ut_parse(utSystem, from.c_str(), UT_UTF8), ut_free);
    handleUdUnitError(ut_get_status(), "'" + from + "'");
    std::shared_ptr<ut_unit> toUnit(ut_parse(utSystem, to.c_str(), UT_UTF8), ut_free);
    handleUdUnitError(ut_get_status(), "'" + to + "'");
    cv_converter* conv = ut_get_converter(fromUnit.get(), toUnit.get());
    handleUdUnitError(ut_get_status(), "'" + from + "' converted to '" +to + "'");
    return std::make_shared<Ud2UnitsConverter>(conv);
#else
    double slope, offset;
    utUnit fromUnit, toUnit;
    handleUdUnitError(utScan(from.c_str(), &fromUnit), from);
    handleUdUnitError(utScan(to.c_str(), &toUnit), to);
    handleUdUnitError(utConvert(&fromUnit, &toUnit, &slope, &offset));
    return std::make_shared<LinearUnitsConverter>(slope, offset);
#endif

}

bool Units::areConvertible(const std::string& unit1, const std::string& unit2) const
{
    LOG4FIMEX(logger, Logger::DEBUG, "test convertibility of " << unit1 << " to " << unit2);
    int areConv = 0;
    ScopedCritical lock(unitsMutex);
#ifdef HAVE_UDUNITS2_H
    try {
        std::shared_ptr<ut_unit> fromUnit(ut_parse(utSystem, unit1.c_str(), UT_UTF8), ut_free);
        handleUdUnitError(ut_get_status(), "'" + unit1 + "'");
        std::shared_ptr<ut_unit> toUnit(ut_parse(utSystem, unit2.c_str(), UT_UTF8), ut_free);
        handleUdUnitError(ut_get_status(), "'" + unit2 + "'");
        areConv = ut_are_convertible(fromUnit.get(), toUnit.get());
    } catch (UnitException& ue) {
        LOG4FIMEX(logger, Logger::WARN, ue.what());
    }
#else
    utUnit fromUnit, toUnit;
    double slope, offset;
    handleUdUnitError(utScan(unit1.c_str(), &fromUnit), unit1);
    handleUdUnitError(utScan(unit2.c_str(), &toUnit), unit2);
    int error = utConvert(&fromUnit, &toUnit, &slope, &offset);
    switch (error) {
    case 0: areConv = 1; break;
    case UT_ECONVERT: areConv = 0; break;
    default: handleUdUnitError(error);
    }
#endif

    return areConv;
}
bool Units::isTime(const std::string& timeUnit) const
{
#ifdef HAVE_UDUNITS2_H
    return areConvertible(timeUnit, "seconds since 1970-01-01 00:00:00");
#else
    bool isTime = false;
    ScopedCritical lock(unitsMutex);
    utUnit unit;
    handleUdUnitError(utScan(timeUnit.c_str(), &unit), timeUnit);
    isTime = (utIsTime(&unit) != 0);
    return isTime;
#endif
}

const void* Units::exposeInternals() const {
#ifdef HAVE_UDUNITS2_H
    return utSystem;
#else
    return 0;
#endif
}

}
