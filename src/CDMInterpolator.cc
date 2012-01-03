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


#ifdef _OPENMP
#include <omp.h>
#endif

// fimex
//
#include "fimex/CDM.h"
#include "fimex/CDMInterpolator.h"
#include "fimex/coordSys/CoordinateSystem.h"
#include "fimex/coordSys/Projection.h"
#include "CachedForwardInterpolation.h"
#include "fimex/CachedInterpolation.h"
#include "fimex/Logger.h"
#include "fimex/SpatialAxisSpec.h"
#include "nanoflann/nanoflann.hpp"
#include "../config.h"
#ifdef HAVE_NETCDF_H
#include "fimex/NetCDF_CDMReader.h"
#endif

// PROJ.4
//
#include "proj_api.h"

// boost
//
#include <boost/regex.hpp>

// standard
#include <functional>
#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <ctime>
#include <fstream>

namespace MetNoFimex
{
using namespace std;

struct CDMInterpolatorInternals {
    boost::shared_ptr<CDMReader> dataReader;
    double maxDistance; // negative = undefined
    std::string latitudeName;
    std::string longitudeName;
    std::vector<boost::shared_ptr<InterpolatorProcess2d> > preprocesses;
    // variableName, horizontalId
    std::map<std::string, std::string> projectionVariables;
    // horizontalId, cachedInterpolation
    map<string, boost::shared_ptr<CachedInterpolationInterface> > cachedInterpolation;
    // horizontalId, cachedVectorReprojection
    map<string, boost::shared_ptr<CachedVectorReprojection> > cachedVectorReprojection;
};

const std::string LAT_LON_PROJSTR = "+proj=latlong +datum=WGS84 +towgs84=0,0,0 +no_defs";

typedef boost::shared_ptr<const CoordinateSystem> CoordSysPtr;

const bool DEBUG = false;
static LoggerPtr logger = getLogger("fimex.CDMInterpolator");

CDMInterpolator::CDMInterpolator(boost::shared_ptr<CDMReader> dataReader)
: p_(new CDMInterpolatorInternals())
{
    p_->dataReader = dataReader;
    p_->maxDistance = -1;
    p_->latitudeName = "lat";
    p_->longitudeName = "lon";
    *cdm_ = p_->dataReader->getCDM();
}

CDMInterpolator::~CDMInterpolator()
{
}

static boost::shared_array<float> data2InterpolationArray(const boost::shared_ptr<Data>& inData, double badValue) {
    boost::shared_array<float> array = inData->asFloat();
    mifi_bad2nanf(&array[0], &array[inData->size()], badValue);
    return array;
}

// for performance reasons, the iData-reference will be modified and used within the return data
static boost::shared_ptr<Data> interpolationArray2Data(boost::shared_array<float> iData, size_t size, double badValue) {
    mifi_nanf2bad(&iData[0], &iData[size], badValue);
    return createData(size, iData);
}
/**
 *
 * run all processes 2d-slice by 2d-slice on the array of size
 *
 * @param processes list of processes
 * @param array the data
 * @param size size of the data (must be N * nx * ny)
 * @param nx size in x-direction
 * @param ny size in y-direction
 */
static void processArray_(vector<boost::shared_ptr<InterpolatorProcess2d> > processes, float* array, size_t size, size_t nx, size_t ny)
{
    if (processes.size() == 0) return; // nothing to do

    int nz = static_cast<int>(size / (nx*ny));
    assert((nz*nx*ny) == size);

#ifdef _OPENMP
#pragma omp parallel default(shared) if (nz >= 4)
    {
#pragma omp for nowait
#endif
    for (int z = 0; z < nz; z++) { // using int instead of size_t because of openMP < 3.0
        // find the start of the slice
        float* arrayPos = array + (z*nx*ny);
        for (size_t i = 0; i < processes.size(); i++) {
            processes[i]->operator()(arrayPos, nx, ny);
        }
    }
#ifdef _OPENMP
    }
#endif
    return;
}

boost::shared_ptr<Data> CDMInterpolator::getDataSlice(const std::string& varName, size_t unLimDimPos)
{
    const CDMVariable& variable = cdm_->getVariable(varName);
    if (variable.hasData()) {
        return getDataSliceFromMemory(variable, unLimDimPos);
    }
    if (p_->projectionVariables.find(varName) == p_->projectionVariables.end()) {
        // no projection, just forward
        return p_->dataReader->getDataSlice(varName, unLimDimPos);
    } else {
        string horizontalId = p_->projectionVariables.find(varName)->second;
        boost::shared_ptr<Data> data = p_->dataReader->getDataSlice(varName, unLimDimPos);
        double badValue = cdm_->getFillValue(varName);
        boost::shared_array<float> array = data2InterpolationArray(data, badValue);
        boost::shared_ptr<CachedInterpolationInterface> ci = p_->cachedInterpolation[horizontalId];
        processArray_(p_->preprocesses, array.get(), data->size(), ci->getInX(), ci->getInY());
        size_t newSize = 0;
        boost::shared_array<float> iArray = ci->interpolateValues(array, data->size(), newSize);
        if (variable.isSpatialVector()) {
            if (p_->cachedVectorReprojection.find(horizontalId) != p_->cachedVectorReprojection.end()) {
                boost::shared_ptr<CachedVectorReprojection> cvr = p_->cachedVectorReprojection[horizontalId];
                // fetch and transpose vector-data
                // transposing needed once for each direction (or caching, but that needs to much memory)
                const std::string& counterpart = variable.getSpatialVectorCounterpart();
                boost::shared_array<float> counterPartArray = data2InterpolationArray(p_->dataReader->getDataSlice(counterpart, unLimDimPos), cdm_->getFillValue(counterpart));
                boost::shared_array<float> counterpartiArray = ci->interpolateValues(counterPartArray, data->size(), newSize);
                const std::string& direction = variable.getSpatialVectorDirection();
                if (direction.find("x") != string::npos || direction.find("longitude") != string::npos) {
                    cvr->reprojectValues(iArray, counterpartiArray, newSize);
                } else if (direction.find("y") != string::npos || direction.find("latitude") != string::npos) {
                    cvr->reprojectValues(counterpartiArray, iArray, newSize);
                } else {
                    throw CDMException("could not find x,longitude,y,latitude direction for vector: " + varName + ", direction: " + direction);
                }
            } else {
                LOG4FIMEX(logger, Logger::WARN, "Cannot reproject vector " << variable.getName());
            }
        }
        return interpolationArray2Data(iArray, newSize, badValue);
    }
}

void CDMInterpolator::setLatitudeName(const std::string& latName) {
    p_->latitudeName = latName;
}
const std::string& CDMInterpolator::getLatitudeName() const {
    return p_->latitudeName;
}
void CDMInterpolator::setLongitudeName(const std::string& lonName) {
    p_->longitudeName = lonName;
}
const std::string& CDMInterpolator::getLongitudeName() const {
    return p_->longitudeName;
}
void CDMInterpolator::setDistanceOfInterest(double dist) {
    p_->maxDistance = dist;
}
double CDMInterpolator::getMaxDistanceOfInterest(const vector<double>& out_x_axis, const vector<double>& out_y_axis, bool isMetric) const
{
    if (p_->maxDistance > 0) return p_->maxDistance;
    // find the max distance of two neighboring points in the output
    // this is the region of influence for a cell
    double factor = 1.;
    if (!isMetric) {
        factor = MIFI_EARTH_RADIUS_M;
    }
    double maxDist = 0;
    {
        double maxX = 0;
        for (size_t i = 0; i < out_x_axis.size() - 1; ++i) {
            maxX = max(factor*abs(out_x_axis[i+1]-out_x_axis[i]), maxX);
        }
        double maxY = 0;
        for (size_t j = 0; j < out_y_axis.size()-1; ++j) {
            maxY = max(factor*abs(out_y_axis[j+1] - out_y_axis[j]), maxY);
        }
        maxDist = max(maxX, maxY);
    }
    return maxDist;
}


static string getProjectionName(const string& proj_input) {
    // get the new projection
    std::string newProj;
    boost::smatch what;
    if (boost::regex_search(proj_input, what, boost::regex("\\+proj=(\\S+)"))) {
        newProj = what[1].str();
    } else {
        throw CDMException("cannot find +proj=... in proj-string: " + proj_input);
    }
    return newProj;
}

void CDMInterpolator::changeProjection(int method, const string& proj_input, const string& out_x_axis, const string& out_y_axis, const string& out_x_axis_unit, const string& out_y_axis_unit, const string& out_x_axis_type, const string& out_y_axis_type)
{
    SpatialAxisSpec xAxisSpec(out_x_axis);
    SpatialAxisSpec yAxisSpec(out_y_axis);
    if (xAxisSpec.requireStartEnd() || yAxisSpec.requireStartEnd()) {
        // detect the bounding box in the final projection
        map<string,CoordSysPtr> coordSysMap = findBestCoordinateSystemsAndProjectionVars(false);
        if (p_->projectionVariables.size() < 1) throw CDMException("could not find variables with coordinate system");
        // just using the bounding box of the first coordinate-system found
        CoordSysPtr coordSys = coordSysMap.begin()->second;
        string longitude = coordSys->findAxisOfType(CoordinateAxis::Lon)->getName();
        string latitude = coordSys->findAxisOfType(CoordinateAxis::Lat)->getName();
        if (latitude == "" || longitude == "") throw CDMException("could not find lat/long variables");
        const vector<string> dims = cdm_->getVariable(latitude).getShape();
        boost::shared_array<double> latVals = p_->dataReader->getScaledData(latitude)->asDouble();
        size_t latSize = p_->dataReader->getData(latitude)->size();
        boost::shared_array<double> lonVals = p_->dataReader->getScaledData(longitude)->asDouble();
        transform(&latVals[0], &latVals[0]+latSize, &latVals[0], bind1st(multiplies<double>(), DEG_TO_RAD));
        transform(&lonVals[0], &lonVals[0]+latSize, &lonVals[0], bind1st(multiplies<double>(), DEG_TO_RAD));
        if (getProjectionName(proj_input) != "latlong") {
            std::string orgProjStr = LAT_LON_PROJSTR;
            if (MIFI_OK != mifi_project_values(orgProjStr.c_str(), proj_input.c_str(), &lonVals[0], &latVals[0], latSize)) {
                throw CDMException("unable to project axes from "+orgProjStr+ " to " +proj_input);
            }
            // lonVals contains now all x-values, latVals all y-values
            // get bounding box:
            double xMin = *(min_element(&lonVals[0], &lonVals[latSize]));
            double yMin = *(min_element(&latVals[0], &latVals[latSize]));
            double xMax = *(max_element(&lonVals[0], &lonVals[latSize]));
            double yMax = *(max_element(&latVals[0], &latVals[latSize]));
            xAxisSpec.setStartEnd(xMin, xMax);
            yAxisSpec.setStartEnd(yMin, yMax);
            LOG4FIMEX(logger, Logger::INFO, "changeProjection, boundingbox: (" <<  xMin << "," << yMin << "), (" << xMax << "," << yMax << ")");
        } else {
            throw CDMException("changeProjection with autotuning axes only implemented for projections in m, not degree yet");
        }
    }

    CDMDataType xType = string2datatype(out_x_axis_type);
    CDMDataType yType = string2datatype(out_y_axis_type);
    changeProjection(method, proj_input, xAxisSpec.getAxisSteps(), yAxisSpec.getAxisSteps(), out_x_axis_unit, out_y_axis_unit, xType, yType);
    vector<double> yAxisSpecV = yAxisSpec.getAxisSteps();
}

void CDMInterpolator::changeProjection(int method, const string& proj_input, const vector<double>& out_x_axis, const vector<double>& out_y_axis, const string& out_x_axis_unit, const string& out_y_axis_unit)
{
    return changeProjection(method, proj_input, out_x_axis, out_y_axis, out_x_axis_unit, out_y_axis_unit, CDM_DOUBLE, CDM_DOUBLE);

}


void CDMInterpolator::changeProjection(int method, const string& proj_input, const vector<double>& out_x_axis, const vector<double>& out_y_axis, const string& out_x_axis_unit, const string& out_y_axis_unit, CDMDataType out_x_axis_type, CDMDataType out_y_axis_type)
{
    if (out_x_axis_type == CDM_NAT ||
            out_y_axis_type == CDM_NAT) {
        throw CDMException("axis type of interpolation not well defined");
    }
    *cdm_ = p_->dataReader->getCDM(); // reset previous changes
    p_->projectionVariables.clear(); // reset variables
    switch (method) {
    case MIFI_INTERPOL_NEAREST_NEIGHBOR:
    case MIFI_INTERPOL_BILINEAR:
    case MIFI_INTERPOL_BICUBIC:
        changeProjectionByProjectionParameters(method, proj_input, out_x_axis, out_y_axis, out_x_axis_unit, out_y_axis_unit, out_x_axis_type, out_y_axis_type); break;
    case MIFI_INTERPOL_COORD_NN:
    case MIFI_INTERPOL_COORD_NN_KD:
        changeProjectionByCoordinates(method, proj_input, out_x_axis, out_y_axis, out_x_axis_unit, out_y_axis_unit, out_x_axis_type, out_y_axis_type); break;
    case MIFI_INTERPOL_FORWARD_SUM:
    case MIFI_INTERPOL_FORWARD_MEAN:
    case MIFI_INTERPOL_FORWARD_MEDIAN:
    case MIFI_INTERPOL_FORWARD_MAX:
    case MIFI_INTERPOL_FORWARD_MIN:
        changeProjectionByForwardInterpolation(method, proj_input, out_x_axis, out_y_axis, out_x_axis_unit, out_y_axis_unit, out_x_axis_type, out_y_axis_type); break;
    default: throw CDMException("unknown projection method: " + type2string(method));
    }
}

void CDMInterpolator::changeProjection(int method, const std::string& netcdf_template_file)
{
#ifdef HAVE_NETCDF_H
    if (!std::ifstream(netcdf_template_file.c_str())) {
        LOG4FIMEX(logger, Logger::WARN, "changeProjection, netcdf_template_file: not found" );
        return;
    }

    *cdm_ = p_->dataReader->getCDM(); // reset previous changes
    p_->projectionVariables.clear();  // reset variables
    p_->cachedInterpolation.clear();
    p_->cachedVectorReprojection.clear();

    switch (method) {
        case MIFI_INTERPOL_NEAREST_NEIGHBOR:
        case MIFI_INTERPOL_BILINEAR:
        case MIFI_INTERPOL_BICUBIC:
        {
           // compile template CDM modell from netcdf_template_file
           boost::shared_ptr<NetCDF_CDMReader> tmplReader(new NetCDF_CDMReader(netcdf_template_file));

           const CDM& tmplCdmRef = tmplReader->getCDM();

           const std::string tmplRefVarName("referenceVariable");

           assert(tmplCdmRef.hasVariable(tmplRefVarName));

           // get lat / lon info
           std::string tmplLatName;
           std::string tmplLonName;
           tmplCdmRef.getLatitudeLongitude(tmplRefVarName, tmplLatName, tmplLonName);
           boost::shared_ptr<Data> tmplLatVals = tmplReader->getData(tmplLatName);
           boost::shared_ptr<Data> tmplLonVals = tmplReader->getData(tmplLonName);

           // get X / Y info
           std::string tmplXName;
           std::string tmplYName;
           tmplXName = tmplCdmRef.getHorizontalXAxis(tmplRefVarName);
           tmplYName = tmplCdmRef.getHorizontalYAxis(tmplRefVarName);
           boost::shared_ptr<Data> tmplXData = tmplReader->getData(tmplXName);
           boost::shared_ptr<Data> tmplYData = tmplReader->getData(tmplYName);
           boost::shared_array<double> tmplXArray = tmplXData->asDouble();
           boost::shared_array<double> tmplYArray = tmplYData->asDouble();
           vector<double> tmplXAxisVec(tmplXArray.get(), tmplXArray.get()+tmplXData->size());
           vector<double> tmplYAxisVec(tmplYArray.get(), tmplYArray.get()+tmplYData->size());

           std::string proj4string(LAT_LON_PROJSTR);

           changeProjectionByProjectionParametersToLatLonTemplate(method,
                                                                  proj4string,
                                                                  tmplXAxisVec,
                                                                  tmplYAxisVec,
                                                                  tmplCdmRef.getUnits(tmplXName),
                                                                  tmplCdmRef.getUnits(tmplYName),
                                                                  tmplCdmRef.getVariable(tmplXName).getDataType(),
                                                                  tmplCdmRef.getVariable(tmplYName).getDataType(),
                                                                  tmplLatVals,
                                                                  tmplLonVals);
           break;
        }
        case MIFI_INTERPOL_COORD_NN:
        case MIFI_INTERPOL_COORD_NN_KD:
        case MIFI_INTERPOL_FORWARD_SUM:
        case MIFI_INTERPOL_FORWARD_MEAN:
        case MIFI_INTERPOL_FORWARD_MEDIAN:
        case MIFI_INTERPOL_FORWARD_MAX:
        case MIFI_INTERPOL_FORWARD_MIN:
            throw CDMException("projection method: " + type2string(method) + ", not supported");
            break;
        default:
            throw CDMException("unknown projection method: " + type2string(method));
    }
#else
    LOG4FIMEX(logger, Logger::ERROR, "fimex not compiled with netcdf-support, can't change projection by netcdf-template" );
    return;
#endif
}

map<string, CoordSysPtr> CDMInterpolator::findBestCoordinateSystemsAndProjectionVars(bool withProjection)
{
    typedef map<string, CoordSysPtr> CoordSysMap;
    typedef vector<CoordSysPtr> CoordSysVec;
    CoordSysMap coordSysMap;
    CoordSysVec coordSys = listCoordinateSystems(getCDM());
    for (CoordSysVec::iterator cs = coordSys.begin(); cs != coordSys.end(); ++cs) {
        if (((!withProjection) || ((*cs)->isSimpleSpatialGridded() && (*cs)->hasProjection())) &&
              (withProjection || ((*cs)->hasAxisType(CoordinateAxis::Lat) && (*cs)->hasAxisType(CoordinateAxis::Lon)))) {
            coordSysMap[(*cs)->horizontalId()] = *cs;
        } else {
            LOG4FIMEX(logger, Logger::DEBUG, "CS dropped: simpleSpatialGrid="<<(*cs)->isSimpleSpatialGridded() << " projection=" << (*cs)->hasProjection() << " lon="<<(*cs)->hasAxisType(CoordinateAxis::Lon)<< " lat="<<(*cs)->hasAxisType(CoordinateAxis::Lat));
        }
    }
    if (coordSysMap.empty()) {
        LOG4FIMEX(logger, Logger::ERROR, "no coordinate-systems" << (withProjection ? " with projection found, maybe you should try coordinate interpolation" : " found"));
        throw CDMException("no coordinate-systems found");
    }
    // mapping all variables with matching orgX/orgY dimensions
    // find all variables belonging to a cs containing the projection
    p_->projectionVariables.clear();
    const CDM::VarVec& vars = getCDM().getVariables();
    for (CDM::VarVec::const_iterator v = vars.begin(); v != vars.end(); ++v) {
        CoordSysVec::iterator vCs = find_if(coordSys.begin(), coordSys.end(), CompleteCoordinateSystemForComparator(v->getName()));
        if (coordSys.end() != vCs) {
            if (coordSysMap.find((*vCs)->horizontalId()) != coordSysMap.end()) {
                p_->projectionVariables[v->getName()] = (*vCs)->horizontalId();
            }
        }
    }
    vector<string> pVars;
    for (map<string, string>::iterator pvi = p_->projectionVariables.begin(); pvi != p_->projectionVariables.end(); ++pvi) {
        pVars.push_back(pvi->first);
    }
    LOG4FIMEX(logger, Logger::DEBUG, "projection variables: " << join(pVars.begin(), pVars.end(), ","));

    // remove all variables which are not part of any coordinate-system
    // but which share geographical-dimensions (x,y,lon,lat) with the system
    for (CoordSysMap::iterator csmi = coordSysMap.begin(); csmi != coordSysMap.end(); ++csmi){
        set<std::string> geoDimensions;
        vector<string>  xShape = getCDM().getVariable(csmi->second->getGeoXAxis()->getName()).getShape();
        vector<string>  yShape = getCDM().getVariable(csmi->second->getGeoYAxis()->getName()).getShape();
        geoDimensions.insert(xShape.begin(), xShape.end());
        geoDimensions.insert(yShape.begin(), yShape.end());
        CDM::VarVec vars = getCDM().getVariables();
        CoordinateSystem::ConstAxisList axes = csmi->second->getAxes();
        for (CDM::VarVec::const_iterator v = vars.begin(); v != vars.end(); ++v) {
            if ((axes.end() == find_if(axes.begin(), axes.end(), CDMNameEqualPtr(v->getName()))) &&
                (p_->projectionVariables.end() == p_->projectionVariables.find(v->getName()))) {
                // v is not an axis
                // v is not a projectionVariable
                vector<string> vShape = getCDM().getVariable(v->getName()).getShape();
                sort(vShape.begin(), vShape.end());
                vector<string> intersect;
                set_intersection(geoDimensions.begin(), geoDimensions.end(),
                                 vShape.begin(), vShape.end(),
                                 back_inserter(intersect));
                if (intersect.size() > 0) {
                    cdm_->removeVariable(v->getName());
                    LOG4FIMEX(logger, Logger::WARN, "removing variable " << v->getName() << " since it is not compatible with the reprojected coordinates");
                }
            }
        }
    }

    CoordSysMap csMapOut;
    // make the coordinateSystems minimal
    for (CoordSysMap::iterator csmi = coordSysMap.begin(); csmi != coordSysMap.end(); ++csmi) {
        boost::shared_ptr<CoordinateSystem> cs(new CoordinateSystem());
        cs->setConventionName(csmi->second->getConventionName());
        cs->setAxis(csmi->second->getGeoXAxis());
        cs->setAxis(csmi->second->getGeoYAxis());
        // find lat and lon, eventually different from GeoX and GeoY

        if (csmi->second->hasProjection()) {
            cs->setProjection(csmi->second->getProjection());
        }
        if (csmi->second->hasAxisType(CoordinateAxis::Lon)) {
            cs->setAxis(csmi->second->findAxisOfType(CoordinateAxis::Lon));
        }
        if (csmi->second->hasAxisType(CoordinateAxis::Lat)) {
            cs->setAxis(csmi->second->findAxisOfType(CoordinateAxis::Lat));
        }

        // add all variables to the coordinate system
        for (map<string, string>::const_iterator v = p_->projectionVariables.begin(); v != p_->projectionVariables.end(); ++v) {
            if (cs->horizontalId() == v->second) {
                cs->setCSFor(v->first);
            }
        }

        LOG4FIMEX(logger,Logger::DEBUG, "interpolator of cs " << *cs);
        csMapOut[csmi->first] = cs;
    }
    if (csMapOut.empty()) {
        LOG4FIMEX(logger, Logger::ERROR, "no coordinate-systems" << (withProjection ? " with projection found, maybe you should try coordinate interpolation" : " found"));
        throw CDMException("no coordinate-systems found");
    }
    for (CoordSysMap::iterator csIt = csMapOut.begin(); csIt != csMapOut.end(); ++csIt) {
        assert(csIt->second.get() != 0);
    }
    return csMapOut;
}

/**
 * make changes in the CDM structure to reflect the new projection (attributes, coordinates, projection-variable, dimensions)
 *
 * @param cmd
 * @param proj_input
 * @param orgProjection
 * @param orgXAxis
 * @param orgYAxis
 */
static void changeCDM(CDM& cdm, const string& proj_input, const map<string, CoordSysPtr>& csMap, const map<string, string>& projectionVariables, const vector<double>& out_x_axis, const vector<double>& out_y_axis, const string& out_x_axis_unit, const string& out_y_axis_unit, CDMDataType xAxisType, CDMDataType yAxisType, const string& longitudeName, const string& latitudeName)
{
    string newProj = getProjectionName(proj_input);
    string newXAxis;
    string newYAxis;
    for (map<string, CoordSysPtr>::const_iterator csi = csMap.begin(); csi != csMap.end(); ++csi) {
        CoordSysPtr cs = csi->second;
        string orgProjection;
        if (cs->hasProjection()) {
            orgProjection = cs->getProjection()->getName();
            // remove all other projections, those might confuse other programs, i.e. IDV
            // this is not int the orgProjection name i.e. stereographic but in the variable name projection_stere
            std::vector<std::string> gridMappings = cdm.findVariables("grid_mapping_name", ".*");
            for (size_t i = 0; i < gridMappings.size(); i++) {
                LOG4FIMEX(logger, Logger::DEBUG, "removing projection-variable " << gridMappings[i]);
                cdm.removeVariable(gridMappings[i]);
            }
            LOG4FIMEX(logger, Logger::DEBUG, "original projection: " << orgProjection);
        }

        string orgXAxis = cs->getGeoXAxis()->getName();
        string orgYAxis = cs->getGeoYAxis()->getName();
        if (cs->hasAxisType(CoordinateAxis::Lon) && (cs->getGeoXAxis()->getName() == cs->findAxisOfType(CoordinateAxis::Lon)->getName())) {
            // x and y axis not properly defined, guessing
            vector<string> lonShape = cdm.getVariable(orgXAxis).getShape();
            if (lonShape.size() == 2) {
                orgXAxis = lonShape[0];
                orgYAxis = lonShape[1];
                LOG4FIMEX(logger, Logger::WARN, "need to guess x and y axis: " << lonShape[0] << " " << lonShape[1]);
            }
        }
        if (newXAxis.size() == 0) {
            newXAxis = orgXAxis;
            newYAxis = orgYAxis;
        } else {
            // use the new x/y-axis in shape for all variables
            for (map<string, string>::const_iterator varIt = projectionVariables.begin(); varIt != projectionVariables.end(); ++varIt) {
                LOG4FIMEX(logger, Logger::DEBUG, "changing shape for newX/YAxis for : " << varIt->first);
                vector<string> shape = cdm.getVariable(varIt->first).getShape();
                replace(shape.begin(), shape.end(), orgXAxis, newXAxis);
                replace(shape.begin(), shape.end(), orgYAxis, newYAxis);
                cdm.getVariable(varIt->first).setShape(shape);
            }
            cdm.removeVariable(orgXAxis);
            cdm.removeVariable(orgYAxis);
        }

        // remove projection and coordinates (lon lat)
        if (cs->hasAxisType(CoordinateAxis::Lat) && cs->hasAxisType(CoordinateAxis::Lon)) {
            typedef CoordinateSystem::ConstAxisPtr ConstAxisPtr;
            ConstAxisPtr latAxis = cs->findAxisOfType(CoordinateAxis::Lat);
            ConstAxisPtr lonAxis = cs->findAxisOfType(CoordinateAxis::Lon);
            LOG4FIMEX(logger, Logger::DEBUG, "removing old coordinate axes " << latAxis->getName() << " and " << lonAxis->getName());
            cdm.removeVariable(latAxis->getName());
            cdm.removeVariable(lonAxis->getName());
        }
        if (orgXAxis != newXAxis) {
            cdm.removeDimension(orgXAxis);
            cdm.removeDimension(orgYAxis);
        }

    }
    assert(newXAxis.size() != 0);
    assert(newYAxis.size() != 0);

    // add new projection and parameters
    std::string newProjection = "latlong";
    if (newProj != "latlong") {
        newProjection = "projection_"+newProj;
        int i = 0;
        while (cdm.hasVariable(newProjection)) newProjection = "projection_"+newProj+type2string(++i);
        CDMVariable projVar(newProjection, CDM_NAT, std::vector<std::string>());
        projVar.setData(createData(CDM_NAT, 0)); // define empty data
        cdm.addVariable(projVar);
        std::vector<CDMAttribute> projAttrs = Projection::createByProj4(proj_input)->getParameters();
        for (std::vector<CDMAttribute>::iterator it = projAttrs.begin(); it != projAttrs.end(); ++it) {
            cdm.addAttribute(newProjection, *it);
        }
    }

    LOG4FIMEX(logger, Logger::DEBUG, "X, Y: " << newXAxis << ", "<< newYAxis);
    LOG4FIMEX(logger, Logger::DEBUG, "new projection: " << newProjection);
    LOG4FIMEX(logger, Logger::DEBUG, "new proj: " << newProj);


    // change/add new axes
    // don't change the name of the dimension, even if this might look strange if e.g. lon is a projection_x_coordinate
    cdm.removeAttribute(newXAxis, "long_name");
    cdm.removeAttribute(newYAxis, "long_name");
    std::string xStandardName;
    std::string yStandardName;
    if (newProj == "latlong") {
        xStandardName = "longitude";
        yStandardName = "latitude";
    } else if (newProjection == "projection_rotated_latitude_longitude") {
        xStandardName = "grid_longitude";
        yStandardName = "grid_latitude";
    } else {
        xStandardName = "projection_x_coordinate";
        yStandardName = "projection_y_coordinate";
    }
    if (!cdm.hasVariable(newXAxis)) {
        // create dimension-variable
        vector<string> shape;
        shape.push_back(newXAxis);
        cdm.addVariable(CDMVariable(newXAxis, xAxisType, shape));
    } else {
        cdm.getVariable(newXAxis).setDataType(xAxisType);
    }
    if (!cdm.hasVariable(newYAxis)) {
        // create dimension-variable
        vector<string> shape;
        shape.push_back(newYAxis);
        cdm.addVariable(CDMVariable(newYAxis, yAxisType, shape));
    } else {
        cdm.getVariable(newYAxis).setDataType(yAxisType);
    }
    cdm.addOrReplaceAttribute(newXAxis, CDMAttribute("standard_name", xStandardName));
    cdm.addOrReplaceAttribute(newYAxis, CDMAttribute("standard_name", yStandardName));
    cdm.addOrReplaceAttribute(newXAxis, CDMAttribute("units", out_x_axis_unit));
    cdm.addOrReplaceAttribute(newYAxis, CDMAttribute("units", out_y_axis_unit));
    cdm.getVariable(newXAxis).setData(createData(CDM_DOUBLE, out_x_axis.begin(), out_x_axis.end()));
    cdm.getVariable(newYAxis).setData(createData(CDM_DOUBLE, out_y_axis.begin(), out_y_axis.end()));

    cdm.getDimension(newXAxis).setLength(out_x_axis.size());
    cdm.getDimension(newYAxis).setLength(out_y_axis.size());

    std::string lat(latitudeName);
    std::string lon(longitudeName);
    if (newProj != "latlong") {
        int i = 0;
        while (cdm.hasVariable(lon)) {
            lon = longitudeName + type2string(++i);
        }
        i = 0;
        while (cdm.hasVariable(lat)) {
            lat = latitudeName + type2string(++i);
        }
        cdm.generateProjectionCoordinates(newProjection, newXAxis, newYAxis, lon, lat);
    }

    // find all reprojectable variables and change variable attributes grid_mapping and coordinates
    {
        for (map<string, string>::const_iterator varIt = projectionVariables.begin(); varIt != projectionVariables.end(); ++varIt) {
            if (newProj != "latlong") {
                cdm.addOrReplaceAttribute(varIt->first, CDMAttribute("coordinates", lon + " " + lat));
                cdm.addOrReplaceAttribute(varIt->first, CDMAttribute("grid_mapping", newProjection));
            } else {
                cdm.removeAttribute(varIt->first, "coordinates");
                cdm.removeAttribute(varIt->first, "grid_mapping");
            }
        }
    }

}

// internal setup for nanoflann kd-tree
template <typename T>
struct PointCloud
{
        struct Point
        {
                T  x,y,z;
        };

        std::vector<Point>  pts;

        // Must return the number of data points
        inline size_t kdtree_get_point_count() const { return pts.size(); }

        // Returns the distance between the vector "p1[0:size-1]" and the data point with index "idx_p2" stored in the class:
        inline T kdtree_distance(const T *p1, const size_t idx_p2, size_t size) const
        {
                T d0=p1[0]-pts[idx_p2].x;
                T d1=p1[1]-pts[idx_p2].y;
                T d2=p1[2]-pts[idx_p2].z;
                return d0*d0+d1*d1+d2*d2;
        }

        // Returns the dim'th component of the idx'th point in the class:
        // Since this is inlined and the "dim" argument is typically an immediate value, the
        //  "if/else's" are actually solved at compile time.
        inline T kdtree_get_pt(const size_t idx, int dim) const
        {
                if (dim==0) return pts[idx].x;
                else if (dim==1) return pts[idx].y;
                else return pts[idx].z;
        }

        // Optional bounding-box computation: return false to default to a standard bbox computation loop.
        //   Return true if the BBOX was already computed by the class and returned in "bb" so it can be avoided to redo it again.
        //   Look at bb.size() to find out the expected dimensionality (e.g. 2 or 3 for point clouds)
        template <class BBOX>
        bool kdtree_get_bbox(BBOX &bb) const { return false; }

};

void flannTranslatePointsToClosestInputCell(double maxDist, vector<double>& pointsOnXAxis, vector<double>& pointsOnYAxis, size_t xAxisSize, size_t yAxisSize, double* lonVals, double* latVals, size_t orgXDimSize, size_t orgYDimSize)
{
    // pointsOnXAxis and pointsOnYAxis as well as lonVals and latVals are now represented in rad

    using namespace nanoflann;
    LOG4FIMEX(logger, Logger::DEBUG, "maximum allowed distance from cell-center: " << maxDist);
    assert(maxDist != 0);

    // all calculations on a sphere with unit 1
    maxDist /= MIFI_EARTH_RADIUS_M;


    time_t start = time(0);
    PointCloud<double> cloud;
    cloud.pts.resize(orgXDimSize*orgYDimSize);
    for (size_t ix = 0; ix < orgXDimSize; ix++) {
        for (size_t iy = 0; iy < orgYDimSize; iy++) {
            size_t pos = ix+iy*orgXDimSize;
            if (!(mifi_isnanf(latVals[pos]) || mifi_isnanf(lonVals[pos]))) {
                double sinLat = sin(latVals[pos]);
                double cosLat = cos(latVals[pos]);
                double sinLon = sin(lonVals[pos]);
                double cosLon = cos(lonVals[pos]);
                cloud.pts[pos].x = cosLat * cosLon;
                cloud.pts[pos].y = cosLat * sinLon;
                cloud.pts[pos].z = sinLat;
            } else {
                cloud.pts[pos].x = MIFI_UNDEFINED_D;
                cloud.pts[pos].y = MIFI_UNDEFINED_D;
                cloud.pts[pos].z = MIFI_UNDEFINED_D;
            }
        }
    }
    // construct a kd-tree index:
    typedef KDTreeSingleIndexAdaptor<L2_Simple_Adaptor<double, PointCloud<double> > ,
                                     PointCloud<double>,
                                     3 /* dim */> my_kd_tree_t;
    my_kd_tree_t index(3 /*dim*/, cloud, KDTreeSingleIndexAdaptorParams(12 /* max leaf */) );
    index.buildIndex();
    LOG4FIMEX(logger, Logger::DEBUG, "finished loading kdTree after " << (time(0) - start) << "s");

    // using square since distance is not sqrt
    const double search_radius = static_cast<double>(maxDist * maxDist);
    nanoflann::SearchParams params;
    params.sorted = true;
    for (size_t i = 0; i < pointsOnXAxis.size(); i++) {
        double sinLat = sin(pointsOnYAxis[i]);
        double cosLat = cos(pointsOnYAxis[i]);
        double sinLon = sin(pointsOnXAxis[i]);
        double cosLon = cos(pointsOnXAxis[i]);
        const double query_pt[3] = { cosLat * cosLon,
                                     cosLat * sinLon,
                                     sinLat };

        std::vector<std::pair<size_t,double> > ret_matches;
        const size_t nMatches = index.radiusSearch(&query_pt[0], search_radius, ret_matches, params);
        if (nMatches > 0) {
            size_t pos = ret_matches.at(0).first; // pos = ix+orgXDimSize*iy =>
            size_t ix = pos % orgXDimSize;
            size_t iy = pos / orgXDimSize;
            //LOG4FIMEX(logger, Logger::DEBUG, "found (" << RAD_TO_DEG*pointsOnXAxis[i] << "," << RAD_TO_DEG*pointsOnYAxis[i] << ") at (" << ix << "," << iy << ") dist: " << ret_matches.at(0).second);
            pointsOnXAxis[i] = ix;
            pointsOnYAxis[i] = iy;
        } else {
            // set to any value outside the axes (0 - x/y-size)
            pointsOnXAxis[i] = -1000;
            pointsOnYAxis[i] = -1000;
        }
    }
    LOG4FIMEX(logger, Logger::DEBUG, "finished flannKDTranslatePointsToClosestInputCell");
}

double getGridDistance(vector<double>& pointsOnXAxis, vector<double>& pointsOnYAxis, double* lonVals, double* latVals, size_t orgXDimSize, size_t orgYDimSize) {
    // try to determine a average grid-distance, take some example points, evaluate the max,
    // multiply that with a number slightly bigger than 1 (i use 1.414
    // and define that as grid distance
    vector<double> samples;
    int steps;
    size_t stepSize;
    if (orgXDimSize * orgYDimSize > 1000) {
        steps = 53; // unusual grid-dimension
        stepSize = orgXDimSize * orgYDimSize / steps;
    } else {
        stepSize = 1;
        steps = orgXDimSize * orgYDimSize;
    }
#ifdef _OPENMP
    omp_lock_t my_lock;
    omp_init_lock(&my_lock);
#pragma omp parallel default(shared) if (steps > 4)
    {
#pragma omp for nowait
#endif
    for (int ik = 0; ik < steps; ik++) { // using int instead of size_t because of openMP < 3.0
        size_t samplePos = ik * stepSize;
        double lon0 = lonVals[samplePos];
        double lat0 = latVals[samplePos];
        if (!(isnan(lon0) || isnan(lat0))) {
            double min_cos_d = -2; // max possible distance on unit-sphere has cos_d -1 -> d= pi * r
            for (size_t ix = 0; ix < orgXDimSize; ix++) {
                for (size_t iy = 0; iy < orgYDimSize; iy++) {
                    // find smallest distance (= max cosinus value): http://en.wikipedia.org/wiki/Great-circle_distance
                    size_t pos = ix+iy*orgXDimSize;
                    if (pos != samplePos) {
                        double lon1 = lonVals[pos];
                        double lat1 = latVals[pos];
                        if (!(isnan(lon1) || isnan(lat1))) {

                            double dlon = lon0 - lon1;

                            double cos_d = cos(lat0) * cos(lat1) * cos(dlon) + sin(lat0) * sin(lat1);
                            if (cos_d > min_cos_d) {
                                min_cos_d = cos_d;
                            }
                        }
                    }
                }
            }
#ifdef _OPENMP
            omp_set_lock (&my_lock);
#endif
            samples.push_back(min_cos_d);
#ifdef _OPENMP
            omp_unset_lock (&my_lock);
#endif
        }
    }
#ifdef _OPENMP
    omp_destroy_lock(&my_lock);
    }
#endif
    double max_grid_d = acos(*(min_element(samples.begin(), samples.end())));
    max_grid_d *= 1.414; // allow a bit larger extrapolation (diagonal = sqrt(2))
    if (max_grid_d > MIFI_PI) max_grid_d = MIFI_PI;
    return max_grid_d;
}

// internal setup for binary search lat/long
class LL_POINT {
public:
    double lat;
    double lon;
    double x;
    double y;
    LL_POINT() : lat(0), lon(0), x(-1.), y(-1.) {}
    LL_POINT(double lat, double lon, double x, double y) : lat(lat), lon(lon), x(x), y(y) {}
    bool operator<(const LL_POINT& rhs) const { return (this->lat < rhs.lat); }
};

// translate all degree-values of pointsOnX/YAxis to the nearest index
// in lonVals/latVals using a binary search in latitude-direction, otherwise brute-force
void fastTranslatePointsToClosestInputCell(vector<double>& pointsOnXAxis, vector<double>& pointsOnYAxis, double* lonVals, double* latVals, size_t orgXDimSize, size_t orgYDimSize)
{
    LOG4FIMEX(logger, Logger::DEBUG, "estimation of ROI of input-data");
    time_t start = time(0);
    double max_grid_d = getGridDistance(pointsOnXAxis, pointsOnYAxis, &lonVals[0], &latVals[0], orgXDimSize, orgYDimSize);
    LOG4FIMEX(logger, Logger::DEBUG, "assuming a ROI of input-data as: "<< (max_grid_d*180/MIFI_PI) << "deg after " << (time(0) - start) << "s");
    double min_grid_cos_d = cos(max_grid_d);

    // 1. order lat/lon after latitude
    // 2. search for closest latitude
    // 3. go up and down in list as long as dlat < min_dist
    std::vector<LL_POINT> latlons;
    latlons.reserve(orgXDimSize*orgYDimSize);
    for (size_t ix = 0; ix < orgXDimSize; ix++) {
        for (size_t iy = 0; iy < orgYDimSize; iy++) {
            size_t pos = ix+iy*orgXDimSize;
            if (!(isnan(lonVals[pos]) || isnan(latVals[pos]))) {
                latlons.push_back(LL_POINT(latVals[pos],lonVals[pos], ix, iy));
            }
        }
    }
    // sort latlons by latitudes
    sort(latlons.begin(), latlons.end());
#ifdef _OPENMP
#pragma omp parallel default(shared)
    {
#pragma omp for nowait
#endif
    for (int i = 0; i < static_cast<int>(pointsOnXAxis.size()); i++) { // using int instead of size_t because of openMP < 3.0
        //                   lat                   lon
        LL_POINT p(pointsOnYAxis[i], pointsOnXAxis[i], -1., -1.);
        size_t steps = 0;
        double min_cos_d = min_grid_cos_d; // max allowed distance
        double min_d = acos(min_cos_d);
        vector<LL_POINT>::const_iterator it = lower_bound(latlons.begin(), latlons.end(), p);
        vector<LL_POINT>::const_iterator it2 = it;
        // loop until end
        while (it != latlons.end()) {
            steps++;
            double dlon = it->lon - p.lon;
            if (fabs(it->lat - p.lat) > min_d) {
                it = latlons.end(); // all successing
            } else {
                double cos_d = cos(it->lat) * cos(p.lat) * cos(dlon) + sin(it->lat) * sin(p.lat);
                if (cos_d > min_cos_d) { // closer distance
                    min_cos_d = cos_d;
                    min_d = acos(min_cos_d);
                    p.x = it->x;
                    p.y = it->y;
                }
                it++;
            }
        }
        // loop until beginning
        if (it2 != latlons.begin()) {
            do {
                steps++;
                it2--;
                double dlon = it2->lon - p.lon;
                if (fabs(it2->lat - p.lat) > min_d) {
                    it2 = latlons.begin(); // all successing
                } else {
                    double cos_d = cos(it2->lat) * cos(p.lat) * cos(dlon) + sin(it2->lat) * sin(p.lat);
                    if (cos_d > min_cos_d) { // closer distance
                        min_cos_d = cos_d;
                        min_d = acos(min_cos_d);
                        p.x = it2->x;
                        p.y = it2->y;
                    }
                }
            } while (it2 != latlons.begin());
        }

        pointsOnYAxis[i] = p.y;
        pointsOnXAxis[i] = p.x;
    }
#ifdef _OPENMP
    }
#endif
}

/**
 * convert latVals and lonVals to matrices
 * @return lonVals in size (lonSize*latSize)
 */
static void lonLatVals2Matrix(boost::shared_array<double>& lonVals, boost::shared_array<double>& latVals, size_t lonSize, size_t latSize)
{
    boost::shared_array<double> matrixLatVals(new double[lonSize * latSize]);
    boost::shared_array<double> matrixLonVals(new double[lonSize * latSize]);
    for (size_t ix = 0; ix < lonSize; ix++) {
        for (size_t iy = 0; iy < latSize; iy++) {
            size_t pos = ix+iy*lonSize;
            matrixLonVals[pos] = lonVals[ix];
            matrixLatVals[pos] = latVals[iy];
        }
    }
    lonVals = matrixLonVals;
    latVals = matrixLatVals;
}

void CDMInterpolator::changeProjectionByForwardInterpolation(int method, const string& proj_input, const vector<double>& out_x_axis, const vector<double>& out_y_axis, const string& out_x_axis_unit, const string& out_y_axis_unit, CDMDataType out_x_axis_type, CDMDataType out_y_axis_type)
{
    map<string,CoordSysPtr> csMap = findBestCoordinateSystemsAndProjectionVars(false);

    changeCDM(*cdm_.get(), proj_input, csMap, p_->projectionVariables,
              out_x_axis, out_y_axis, out_x_axis_unit, out_y_axis_unit,
              out_x_axis_type, out_y_axis_type,
              getLongitudeName(), getLatitudeName());

    for (map<string,CoordSysPtr>::iterator csIt = csMap.begin(); csIt != csMap.end(); ++csIt) {
        CoordSysPtr cs = csIt->second;
        string latitude = cs->findAxisOfType(CoordinateAxis::Lat)->getName();
        string longitude = cs->findAxisOfType(CoordinateAxis::Lon)->getName();
        boost::shared_array<double> latVals = p_->dataReader->getScaledData(latitude)->asDouble();
        size_t latSize = p_->dataReader->getData(latitude)->size();
        boost::shared_array<double> lonVals = p_->dataReader->getScaledData(longitude)->asDouble();
        transform(&latVals[0], &latVals[0]+latSize, &latVals[0], bind1st(multiplies<double>(), DEG_TO_RAD));
        transform(&lonVals[0], &lonVals[0]+latSize, &lonVals[0], bind1st(multiplies<double>(), DEG_TO_RAD));

        size_t orgXDimSize;
        size_t orgYDimSize;
        bool latLonProj = (cs->hasProjection() && (cs->getProjection()->getName() == "latitude_longitude"));
        if (latLonProj) {
            orgXDimSize = p_->dataReader->getCDM().getDimension(cs->getGeoXAxis()->getName()).getLength();
            orgYDimSize = p_->dataReader->getCDM().getDimension(cs->getGeoYAxis()->getName()).getLength();
            // create new latVals and lonVals as a matrix
            lonLatVals2Matrix(lonVals, latVals, orgXDimSize, orgYDimSize);
            latSize = orgXDimSize * orgYDimSize;
        } else {
            if (cs->getGeoYAxis()->getName() == latitude) {
                // x and y axis not properly defined, guessing
                vector<string> latShape = p_->dataReader->getCDM().getVariable(latitude).getShape();
                if (latShape.size() != 2) {
                    throw CDMException("latitude needs 2 dims for forward interpolation");
                }
                orgXDimSize = p_->dataReader->getCDM().getDimension(latShape[0]).getLength();
                orgYDimSize = p_->dataReader->getCDM().getDimension(latShape[1]).getLength();
                LOG4FIMEX(logger, Logger::DEBUG, "x and y axis: " << latShape[0] << "(" << orgXDimSize << "), " << latShape[1] << "(" << orgYDimSize << ")");
            } else {
                orgXDimSize = p_->dataReader->getCDM().getDimension(cs->getGeoXAxis()->getName()).getLength();
                orgYDimSize = p_->dataReader->getCDM().getDimension(cs->getGeoYAxis()->getName()).getLength();
            }
        }


        // store projection changes to be used in data-section
        // translate temporary new axes from deg2rad if required
        int miupXAxis = MIFI_PROJ_AXIS;
        int miupYAxis = MIFI_PROJ_AXIS;
        vector<double> outXAxis = out_x_axis;
        vector<double> outYAxis = out_y_axis;
        boost::regex degree(".*degree.*");
        if (boost::regex_match(out_x_axis_unit, degree)) {
            transform(outXAxis.begin(), outXAxis.end(), outXAxis.begin(), bind1st(multiplies<double>(), DEG_TO_RAD));
            miupXAxis = MIFI_LONGITUDE;
        }
        if (boost::regex_match(out_y_axis_unit, degree)) {
            transform(outYAxis.begin(), outYAxis.end(), outYAxis.begin(), bind1st(multiplies<double>(), DEG_TO_RAD));
            miupYAxis = MIFI_LATITUDE;
        }

        // translate all input points to output-coordinates, stored in lonVals and latVals
        std::string orgProjStr = LAT_LON_PROJSTR;
        if (MIFI_OK != mifi_project_values(orgProjStr.c_str(), proj_input.c_str(), &lonVals[0], &latVals[0], latSize)) {
            throw CDMException("unable to project axes from "+proj_input+ " to " +orgProjStr);
        }

        // translate the converted input-coordinates (lonvals and latvals) to cell-positions in output
        mifi_points2position(&lonVals[0], latSize, &outXAxis[0], outXAxis.size(), miupXAxis);
        mifi_points2position(&latVals[0], latSize, &outYAxis[0], outYAxis.size(), miupYAxis);

        //convert lonVals and latVals to vector
        vector<double> pointsOnXAxis, pointsOnYAxis;
        pointsOnXAxis.reserve(latSize);
        pointsOnYAxis.reserve(latSize);
        copy(&lonVals[0], &lonVals[0]+latSize, back_inserter(pointsOnXAxis));
        copy(&latVals[0], &latVals[0]+latSize, back_inserter(pointsOnYAxis));

        // store the interpolation
        LOG4FIMEX(logger, Logger::DEBUG, "creating cached forward interpolation matrix " << orgXDimSize << "x" << orgYDimSize << " => " << out_x_axis.size() << "x" << out_y_axis.size());
        p_->cachedInterpolation[csIt->first] = boost::shared_ptr<CachedInterpolationInterface>(new CachedForwardInterpolation(method, pointsOnXAxis, pointsOnYAxis, orgXDimSize, orgYDimSize, out_x_axis.size(), out_y_axis.size()));
    }
    if (hasSpatialVectors()) {
        LOG4FIMEX(logger, Logger::WARN, "vector data found, but not possible to interpolate with forward-interpolation");
    }

}

void CDMInterpolator::changeProjectionByCoordinates(int method, const string& proj_input, const vector<double>& out_x_axis, const vector<double>& out_y_axis, const string& out_x_axis_unit, const string& out_y_axis_unit, CDMDataType out_x_axis_type, CDMDataType out_y_axis_type)
{
    map<string,CoordSysPtr> csMap = findBestCoordinateSystemsAndProjectionVars(false);

    changeCDM(*cdm_.get(), proj_input, csMap, p_->projectionVariables,
              out_x_axis, out_y_axis, out_x_axis_unit, out_y_axis_unit,
              out_x_axis_type, out_y_axis_type,
              getLongitudeName(), getLatitudeName());

    for (map<string,CoordSysPtr>::iterator csIt = csMap.begin(); csIt != csMap.end(); ++csIt) {
        CoordSysPtr cs = csIt->second;
        string latitude = cs->findAxisOfType(CoordinateAxis::Lat)->getName();
        string longitude = cs->findAxisOfType(CoordinateAxis::Lon)->getName();
        boost::shared_array<double> latVals = p_->dataReader->getScaledData(latitude)->asDouble();
        size_t latSize = p_->dataReader->getData(latitude)->size();
        boost::shared_array<double> lonVals = p_->dataReader->getScaledData(longitude)->asDouble();
        transform(&latVals[0], &latVals[0]+latSize, &latVals[0], bind1st(multiplies<double>(), DEG_TO_RAD));
        transform(&lonVals[0], &lonVals[0]+latSize, &lonVals[0], bind1st(multiplies<double>(), DEG_TO_RAD));

        size_t orgXDimSize;
        size_t orgYDimSize;
        bool latLonProj = (cs->hasProjection() && (cs->getProjection()->getName() == "latitude_longitude"));
        if (latLonProj) {
            orgXDimSize = p_->dataReader->getCDM().getDimension(cs->getGeoXAxis()->getName()).getLength();
            orgYDimSize = p_->dataReader->getCDM().getDimension(cs->getGeoYAxis()->getName()).getLength();
            // create new latVals and lonVals as a matrix
            lonLatVals2Matrix(lonVals, latVals, orgXDimSize, orgYDimSize);
            latSize = orgXDimSize * orgYDimSize;
        } else {
            if (cs->getGeoYAxis()->getName() == latitude) {
                // x and y axis not properly defined, guessing
                vector<string> latShape = p_->dataReader->getCDM().getVariable(latitude).getShape();
                if (latShape.size() != 2) {
                    throw CDMException("latitude needs 2 dims for forward interpolation");
                }
                LOG4FIMEX(logger, Logger::DEBUG, "x and y axis: " << latShape[0] << "," << latShape[1]);
                orgXDimSize = p_->dataReader->getCDM().getDimension(latShape[0]).getLength();
                orgYDimSize = p_->dataReader->getCDM().getDimension(latShape[1]).getLength();
            } else {
                LOG4FIMEX(logger, Logger::DEBUG, "x and y axis: " << cs->getGeoXAxis()->getName() << "," << cs->getGeoYAxis()->getName());
                orgXDimSize = p_->dataReader->getCDM().getDimension(cs->getGeoXAxis()->getName()).getLength();
                orgYDimSize = p_->dataReader->getCDM().getDimension(cs->getGeoYAxis()->getName()).getLength();
            }
        }

        // store projection changes to be used in data-section
        // translate temporary new axes from deg2rad if required
        vector<double> outXAxis = out_x_axis;
        vector<double> outYAxis = out_y_axis;
        boost::regex degree(".*degree.*");
        bool isMetric = true;
        if (boost::regex_match(out_x_axis_unit, degree)) {
            isMetric = false;
            transform(outXAxis.begin(), outXAxis.end(), outXAxis.begin(), bind1st(multiplies<double>(), DEG_TO_RAD));
        }
        if (boost::regex_match(out_y_axis_unit, degree)) {
            transform(outYAxis.begin(), outYAxis.end(), outYAxis.begin(), bind1st(multiplies<double>(), DEG_TO_RAD));
        }
        // get output axes expressed in latitude, longitude
        size_t fieldSize = outXAxis.size() * outYAxis.size();
        vector<double> pointsOnXAxis(fieldSize);
        vector<double> pointsOnYAxis(fieldSize);
        std::string orgProjStr = LAT_LON_PROJSTR;
        if (MIFI_OK != mifi_project_axes(proj_input.c_str(), orgProjStr.c_str(), &outXAxis[0], &outYAxis[0], outXAxis.size(), outYAxis.size(), &pointsOnXAxis[0], &pointsOnYAxis[0])) {
            throw CDMException("unable to project axes from "+orgProjStr+ " to " +proj_input.c_str());
        }
        if (method == MIFI_INTERPOL_COORD_NN) {
            fastTranslatePointsToClosestInputCell(pointsOnXAxis, pointsOnYAxis, &lonVals[0], &latVals[0], orgXDimSize, orgYDimSize);
        } else if (method == MIFI_INTERPOL_COORD_NN_KD) {
            double maxDistance = getMaxDistanceOfInterest(out_x_axis, out_y_axis, isMetric);
            flannTranslatePointsToClosestInputCell(maxDistance, pointsOnXAxis, pointsOnYAxis, outXAxis.size(), outYAxis.size(), &lonVals[0], &latVals[0], orgXDimSize, orgYDimSize);
        } else {
            throw CDMException("unkown interpolation method for coordinates: " + type2string(method));
        }

        LOG4FIMEX(logger, Logger::DEBUG, "creating cached coordinate interpolation matrix " << orgXDimSize << "x" << orgYDimSize << " => " << out_x_axis.size() << "x" << out_y_axis.size());
        p_->cachedInterpolation[csIt->first] = boost::shared_ptr<CachedInterpolationInterface>(new CachedInterpolation(method, pointsOnXAxis, pointsOnYAxis, orgXDimSize, orgYDimSize, out_x_axis.size(), out_y_axis.size()));
    }
    if (hasSpatialVectors()) {
        LOG4FIMEX(logger, Logger::WARN, "vector data found, but not possible? to interpolate with coordinate-interpolation");
    }
}

void CDMInterpolator::changeProjectionByProjectionParameters(int method, const string& proj_input, const vector<double>& out_x_axis, const vector<double>& out_y_axis, const string& out_x_axis_unit, const string& out_y_axis_unit, CDMDataType out_x_axis_type, CDMDataType out_y_axis_type)
{
    map<string, CoordSysPtr> csMap = findBestCoordinateSystemsAndProjectionVars(true);
    changeCDM(*cdm_.get(), proj_input, csMap, p_->projectionVariables,
              out_x_axis, out_y_axis, out_x_axis_unit, out_y_axis_unit,
              out_x_axis_type, out_y_axis_type,
              getLongitudeName(), getLatitudeName());

    for (map<string,CoordSysPtr>::iterator csIt = csMap.begin(); csIt != csMap.end(); ++csIt) {
        CoordSysPtr cs = csIt->second;
        // translate axes to 'm' if given in other metric units
        std::string orgUnit = cs->getProjection()->isDegree() ? "degree" : "m";
        boost::shared_ptr<Data> orgXAxisVals = p_->dataReader->getScaledDataInUnit(cs->getGeoXAxis()->getName(), orgUnit);
        boost::shared_ptr<Data> orgYAxisVals = p_->dataReader->getScaledDataInUnit(cs->getGeoYAxis()->getName(), orgUnit);

        // store projection changes to be used in data-section
        // translate temporary new axes from deg2rad if required
        vector<double> outXAxis = out_x_axis;
        vector<double> outYAxis = out_y_axis;
        boost::regex degree(".*degree.*");
        if (boost::regex_match(out_x_axis_unit, degree)) {
            transform(outXAxis.begin(), outXAxis.end(), outXAxis.begin(), bind1st(multiplies<double>(), DEG_TO_RAD));
        }
        if (boost::regex_match(out_y_axis_unit, degree)) {
            transform(outYAxis.begin(), outYAxis.end(), outYAxis.begin(), bind1st(multiplies<double>(), DEG_TO_RAD));
        }

        // calculate the mapping from the new projection points to the original axes pointsOnXAxis(x_new, y_new), pointsOnYAxis(x_new, y_new)
        size_t fieldSize = outXAxis.size() * outYAxis.size();
        vector<double> pointsOnXAxis(fieldSize);
        vector<double> pointsOnYAxis(fieldSize);
        std::string orgProjStr = cs->getProjection()->getProj4String();
        if (MIFI_OK != mifi_project_axes(proj_input.c_str(), orgProjStr.c_str(), &outXAxis[0], &outYAxis[0], outXAxis.size(), outYAxis.size(), &pointsOnXAxis[0], &pointsOnYAxis[0])) {
            throw CDMException("unable to project axes from "+orgProjStr+ " to " +proj_input.c_str());
        }
        LOG4FIMEX(logger, Logger::DEBUG, "mifi_project_axes: "<< proj_input << "," << orgProjStr << "," << outXAxis[0] << "," << outYAxis[0] << " => " << pointsOnXAxis[0] << "," << pointsOnYAxis[0]);

        // translate original axes from deg2rad if required
        int miupXAxis = MIFI_PROJ_AXIS;
        int miupYAxis = MIFI_PROJ_AXIS;
        boost::shared_array<double> orgXAxisValsArray = orgXAxisVals->asDouble();
        boost::shared_array<double> orgYAxisValsArray = orgYAxisVals->asDouble();
        if (cs->getProjection()->isDegree()) {
            miupXAxis = MIFI_LONGITUDE;
            transform(&orgXAxisValsArray[0], &orgXAxisValsArray[0]+orgXAxisVals->size(), &orgXAxisValsArray[0], bind1st(multiplies<double>(), DEG_TO_RAD));
            miupYAxis = MIFI_LATITUDE;
            transform(&orgYAxisValsArray[0], &orgYAxisValsArray[0]+orgYAxisVals->size(), &orgYAxisValsArray[0], bind1st(multiplies<double>(), DEG_TO_RAD));
        }
        // translate coordinates (in rad or m) to indices
        mifi_points2position(&pointsOnXAxis[0], fieldSize, orgXAxisValsArray.get(), orgXAxisVals->size(), miupXAxis);
        mifi_points2position(&pointsOnYAxis[0], fieldSize, orgYAxisValsArray.get(), orgYAxisVals->size(), miupYAxis);

        LOG4FIMEX(logger, Logger::DEBUG, "creating cached projection interpolation matrix " << orgXAxisVals->size() << "x" << orgYAxisVals->size() << " => " << out_x_axis.size() << "x" << out_y_axis.size());
        p_->cachedInterpolation[csIt->first] = boost::shared_ptr<CachedInterpolationInterface>(new CachedInterpolation(method, pointsOnXAxis, pointsOnYAxis, orgXAxisVals->size(), orgYAxisVals->size(), out_x_axis.size(), out_y_axis.size()));
        LOG4FIMEX(logger, Logger::DEBUG, "...created");

        if (hasSpatialVectors()) {
            // prepare interpolation of vectors
            LOG4FIMEX(logger, Logger::DEBUG, "creating cached vector projection interpolation matrix " << orgXAxisVals->size() << "x" << orgYAxisVals->size() << " => " << out_x_axis.size() << "x" << out_y_axis.size());
            boost::shared_array<double> matrix(new double[out_x_axis.size() * out_y_axis.size() * 4]);
            mifi_get_vector_reproject_matrix(orgProjStr.c_str(), proj_input.c_str(), &out_x_axis[0], &out_y_axis[0], miupXAxis, miupYAxis, out_x_axis.size(), out_y_axis.size(), matrix.get());
            LOG4FIMEX(logger, Logger::DEBUG, "creating vector reprojection");
            p_->cachedVectorReprojection[csIt->first] = boost::shared_ptr<CachedVectorReprojection>(new CachedVectorReprojection(MIFI_VECTOR_KEEP_SIZE, matrix, out_x_axis.size(), out_y_axis.size()));
        }
    }
}

static void changeCDMToLatLonTemplate(CDM& cdm,
                                      const string& tmpl_proj_input,
                                      const CoordSysPtr& cs,
                                      const map<string, string>& projectionVariables,
                                      const vector<double>& out_x_axis,
                                      const vector<double>& out_y_axis,
                                      const string& out_x_axis_unit,
                                      const string& out_y_axis_unit,
                                      CDMDataType xAxisType,
                                      CDMDataType yAxisType,
                                      boost::shared_ptr<Data> tmplLatVals,
                                      boost::shared_ptr<Data> tmplLonVals)
{
    string newProjection = getProjectionName(tmpl_proj_input);

    // expecting latlon - test
    assert(newProjection == "latlong");

    string orgProjection;
    if (cs->hasProjection()) {
        orgProjection = cs->getProjection()->getName();
        // remove all other projections, those might confuse other programs, i.e. IDV
        // this is not int the orgProjection name i.e. stereographic but in the variable name projection_stere
        std::vector<std::string> gridMappings = cdm.findVariables("grid_mapping_name", ".*");
        for (size_t i = 0; i < gridMappings.size(); i++) {
            LOG4FIMEX(logger, Logger::DEBUG, "removing projection-variable " << gridMappings[i]);
            cdm.removeVariable(gridMappings[i]);
        }
    }

    string orgXAxis = cs->getGeoXAxis()->getName();
    string orgYAxis = cs->getGeoYAxis()->getName();
    if (cs->hasAxisType(CoordinateAxis::Lon) && (cs->getGeoXAxis()->getName() == cs->findAxisOfType(CoordinateAxis::Lon)->getName())) {
        // x and y axis not properly defined, guessing
        vector<string> lonShape = cdm.getVariable(orgXAxis).getShape();
        if (lonShape.size() == 2) {
            orgXAxis = lonShape[0];
            orgYAxis = lonShape[1];
            LOG4FIMEX(logger, Logger::WARN, "need to guess x and y axis: " << lonShape[0] << " " << lonShape[1]);
        }
    }

    // remove projection and coordinates (lon lat)
    if (cs->hasAxisType(CoordinateAxis::Lat) && cs->hasAxisType(CoordinateAxis::Lon)) {
        typedef CoordinateSystem::ConstAxisPtr ConstAxisPtr;
        ConstAxisPtr latAxis = cs->findAxisOfType(CoordinateAxis::Lat);
        ConstAxisPtr lonAxis = cs->findAxisOfType(CoordinateAxis::Lon);
        LOG4FIMEX(logger, Logger::DEBUG, "removing old coordinate axes " << latAxis->getName() << " and " << lonAxis->getName());
        cdm.removeVariable(latAxis->getName());
        cdm.removeVariable(lonAxis->getName());
    }

    // add new projection and parameters
    LOG4FIMEX(logger, Logger::DEBUG, "orgX, orgY: " << orgXAxis << ", "<< orgYAxis);
    LOG4FIMEX(logger, Logger::DEBUG, "original projection: " << orgProjection);
    LOG4FIMEX(logger, Logger::DEBUG, "new projection: " << newProjection);

    // change/add new axes
    // don't change the name of the dimension, even if this might look strange if e.g. lon is a projection_x_coordinate
    cdm.removeAttribute(orgXAxis, "long_name");
    cdm.removeAttribute(orgYAxis, "long_name");

    std::string xTemplAxis("x");
    std::string xStandardName("projection_x_coordinate");
    std::string xLongName("x-coordinate in Cartesian system");
    std::string xAxisAtt("x");
    std::string yTemplAxis("y");
    std::string yStandardName("projection_y_coordinate");
    std::string yLongName("y-coordinate in Cartesian system");
    std::string yAxisAtt("y");

    if (!cdm.hasVariable(xTemplAxis)) {
        // create dimension-variable
        vector<string> shape;
        shape.push_back(xTemplAxis);
        cdm.addVariable(CDMVariable(xTemplAxis, xAxisType, shape));
        cdm.addOrReplaceAttribute(xTemplAxis, CDMAttribute("axis", xAxisAtt));
        cdm.addOrReplaceAttribute(xTemplAxis, CDMAttribute("long_name", xLongName));
        cdm.addOrReplaceAttribute(xTemplAxis, CDMAttribute("standard_name", xStandardName));
        cdm.addOrReplaceAttribute(xTemplAxis, CDMAttribute("units", out_x_axis_unit));
        cdm.getVariable(xTemplAxis).setData(createData(CDM_DOUBLE, out_x_axis.begin(), out_x_axis.end()));
        cdm.addDimension(CDMDimension(xTemplAxis, out_x_axis.size()));
    } else {
        cdm.getVariable(orgXAxis).setDataType(xAxisType);
        cdm.addOrReplaceAttribute(orgXAxis, CDMAttribute("axis", xAxisAtt));
        cdm.addOrReplaceAttribute(orgXAxis, CDMAttribute("long_name", xLongName));
        cdm.addOrReplaceAttribute(orgXAxis, CDMAttribute("standard_name", xStandardName));
        cdm.addOrReplaceAttribute(orgXAxis, CDMAttribute("units", out_x_axis_unit));
        cdm.getVariable(orgXAxis).setData(createData(CDM_DOUBLE, out_x_axis.begin(), out_x_axis.end()));
        cdm.getDimension(orgXAxis).setLength(out_x_axis.size());
    }

    if (!cdm.hasVariable(yTemplAxis)) {
        // create dimension-variable
        vector<string> shape;
        shape.push_back(yTemplAxis);
        cdm.addVariable(CDMVariable(yTemplAxis, yAxisType, shape));
        cdm.addOrReplaceAttribute(yTemplAxis, CDMAttribute("axis", yAxisAtt));
        cdm.addOrReplaceAttribute(yTemplAxis, CDMAttribute("long_name", yLongName));
        cdm.addOrReplaceAttribute(yTemplAxis, CDMAttribute("standard_name", yStandardName));
        cdm.addOrReplaceAttribute(yTemplAxis, CDMAttribute("units", out_y_axis_unit));
        cdm.getVariable(yTemplAxis).setData(createData(CDM_DOUBLE, out_y_axis.begin(), out_y_axis.end()));
        cdm.addDimension(CDMDimension(yTemplAxis, out_y_axis.size()));
    } else {
        cdm.getVariable(orgYAxis).setDataType(yAxisType);
        cdm.addOrReplaceAttribute(orgYAxis, CDMAttribute("axis", yAxisAtt));
        cdm.addOrReplaceAttribute(orgYAxis, CDMAttribute("long_name", yLongName));
        cdm.addOrReplaceAttribute(orgYAxis, CDMAttribute("standard_name", yStandardName));
        cdm.addOrReplaceAttribute(orgYAxis, CDMAttribute("units", out_y_axis_unit));
        cdm.getVariable(orgYAxis).setData(createData(CDM_DOUBLE, out_y_axis.begin(), out_y_axis.end()));
        cdm.getDimension(orgYAxis).setLength(out_y_axis.size());
    }

    // adding lat & long from list
    vector<string> shape;
    shape.push_back("x");
    shape.push_back("y");

    // check if the latitude and longitude
    // were already part of the modell
    if(!cdm.hasVariable("latitude"))
        cdm.addVariable(CDMVariable("latitude", CDM_FLOAT, shape));

    cdm.addOrReplaceAttribute("latitude", CDMAttribute("standard_name", "latitude"));
    cdm.addOrReplaceAttribute("latitude", CDMAttribute("long_name", "latitude"));
    cdm.addOrReplaceAttribute("latitude", CDMAttribute("units", "degree_north"));
    cdm.getVariable("latitude").setData(createData(tmplLatVals->size(), tmplLatVals->asFloat()));

    if(!cdm.hasVariable("longitude"))
        cdm.addVariable(CDMVariable("longitude", CDM_FLOAT, shape));

    cdm.addOrReplaceAttribute("longitude", CDMAttribute("standard_name", "longitude"));
    cdm.addOrReplaceAttribute("longitude", CDMAttribute("long_name", "longitude"));
    cdm.addOrReplaceAttribute("longitude", CDMAttribute("units", "degree_east"));
    cdm.getVariable("longitude").setData(createData(tmplLonVals->size(), tmplLonVals->asFloat()));
    // find all reprojectable variables and change variable attributes grid_mapping and coordinates
    {
        for (map<string, string>::const_iterator varIt = projectionVariables.begin(); varIt != projectionVariables.end(); ++varIt) {
            CDMVariable& varRef = cdm.getVariable(varIt->first);
            vector<string> shape = varRef.getShape();
            std::replace(shape.begin(), shape.end(), orgXAxis, xTemplAxis);
            std::replace(shape.begin(), shape.end(), orgYAxis, yTemplAxis);
            varRef.setShape(shape);
            cdm.addOrReplaceAttribute(varIt->first, CDMAttribute("coordinates", "longitude latitude"));
            cdm.removeAttribute(varIt->first, "grid_mapping");
        }
    }
    if(cdm.hasDimension("longitude")) {
        cdm.removeDimension("longitude");
    }
    if(xTemplAxis != "latitude") {
        cdm.removeDimension("latitude");
    }
}

void CDMInterpolator::changeProjectionByProjectionParametersToLatLonTemplate(int method,
                                                                             const string& tmpl_proj_input,
                                                                             const vector<double>& out_x_axis,
                                                                             const vector<double>& out_y_axis,
                                                                             const string& out_x_axis_unit,
                                                                             const string& out_y_axis_unit,
                                                                             CDMDataType out_x_axis_type,
                                                                             CDMDataType out_y_axis_type,
                                                                             boost::shared_ptr<Data> tmplLatVals,
                                                                             boost::shared_ptr<Data> tmplLonVals)
{
    map<string, CoordSysPtr> csMap = findBestCoordinateSystemsAndProjectionVars(true);

    if (csMap.size() > 1) {
        throw CDMException("template-interpolation support only one input horizontal grid, but found several");
    }

    CoordSysPtr cs = csMap.begin()->second;

    // copy all the data you might need before CDM model changes
    // and you will definitely save on debugging time
    boost::shared_ptr<Data> orgXAxisData = getData(cs->getGeoXAxis()->getName());
    boost::shared_ptr<Data> orgYAxisData = getData(cs->getGeoYAxis()->getName());
    boost::shared_array<double> orgXAxisArray = orgXAxisData->asDouble();
    boost::shared_array<double> orgYAxisArray = orgYAxisData->asDouble();

    changeCDMToLatLonTemplate(*cdm_.get(),
                              tmpl_proj_input,
                              cs,
                              p_->projectionVariables,
                              out_x_axis,
                              out_y_axis,
                              out_x_axis_unit,
                              out_y_axis_unit,
                              out_x_axis_type,
                              out_y_axis_type,
                              tmplLatVals,
                              tmplLonVals);

    assert(tmplLatVals->size() == tmplLonVals->size());

    // store projection changes to be used in data-section
    // as template data is in degrees we have to do deg2rad
    boost::shared_array<double> tmplLatArray = tmplLatVals->asDouble();
    boost::shared_array<double> tmplLonArray = tmplLonVals->asDouble();
    vector<double> latY(tmplLatArray.get(), tmplLatArray.get()+tmplLatVals->size());
    vector<double> lonX(tmplLonArray.get(), tmplLonArray.get()+tmplLonVals->size());
    transform(&latY[0], &latY[0]+tmplLatVals->size(), &latY[0], bind1st(multiplies<double>(), DEG_TO_RAD));
    transform(&lonX[0], &lonX[0]+tmplLonVals->size(), &lonX[0], bind1st(multiplies<double>(), DEG_TO_RAD));

    // calculate the mapping from the new projection points to the original axes pointsOnXAxis(x_new, y_new), pointsOnYAxis(x_new, y_new)
    std::string orgProjStr = cs->getProjection()->getProj4String();

    // projects lat / lon from template to axis-projection found in model file
    // we want to get template lat/long expressed in terms of the original projection
    if (MIFI_OK != mifi_project_values(tmpl_proj_input.c_str(), orgProjStr.c_str(), &lonX[0], &latY[0], tmplLatVals->size())) {
        throw CDMException("unable to project values from "+orgProjStr+ " to " +tmpl_proj_input.c_str());
    }
    LOG4FIMEX(logger, Logger::DEBUG, "mifi_project_values: "<< tmpl_proj_input << "," << orgProjStr << "," << out_x_axis[0] << "," << out_y_axis[0] << " => " << lonX[0] << "," << latY[0]);

    // now latVals and lonVals are given in original-input coordinates
    // check if we have to translate original axes from deg2rad
    int miupXAxis = MIFI_PROJ_AXIS;
    int miupYAxis = MIFI_PROJ_AXIS;

    if (cs->getProjection()->isDegree()) {
        miupXAxis = MIFI_LONGITUDE;
        transform(orgXAxisArray.get(), orgXAxisArray.get()+orgXAxisData->size(), orgXAxisArray.get(), bind1st(multiplies<double>(), DEG_TO_RAD));
        miupYAxis = MIFI_LATITUDE;
        transform(orgYAxisArray.get(), orgYAxisArray.get()+orgYAxisData->size(), orgYAxisArray.get(), bind1st(multiplies<double>(), DEG_TO_RAD));
    }

    // translate coordinates (in radians) to indices
    mifi_points2position(&latY[0], tmplLatVals->size(), orgYAxisArray.get(), orgYAxisData->size(), miupYAxis);
    mifi_points2position(&lonX[0], tmplLonVals->size(), orgXAxisArray.get(), orgXAxisData->size(), miupXAxis);

    LOG4FIMEX(logger, Logger::DEBUG, "creating cached projection interpolation matrix " << orgXAxisData->size() << "x" << orgYAxisData->size() << " => " << out_x_axis.size() << "x" << out_y_axis.size());
    p_->cachedInterpolation[csMap.begin()->first] = boost::shared_ptr<CachedInterpolationInterface>(new CachedInterpolation(method,
                                                                                                                            lonX,
                                                                                                                            latY,
                                                                                                                            orgXAxisData->size(),
                                                                                                                            orgYAxisData->size(),
                                                                                                                            out_x_axis.size(),
                                                                                                                            out_y_axis.size()));

    if (hasSpatialVectors()) {
        // prepare interpolation of vectors
        LOG4FIMEX(logger, Logger::WARN, "spatial vectors not implemented for translation to lat-lon list");
    }
}

bool CDMInterpolator::hasSpatialVectors() const
{
    const CDM::VarVec& variables = getCDM().getVariables();
    for (CDM::VarVec::const_iterator varIt = variables.begin(); varIt != variables.end(); ++varIt) {
        if (p_->projectionVariables.find(varIt->getName()) != p_->projectionVariables.end()) {
            if (varIt->isSpatialVector()) {
                return true;
            }
        }
    }
    return false;
}

void CDMInterpolator::addPreprocess(boost::shared_ptr<InterpolatorProcess2d> process)
{
    LOG4FIMEX(logger, Logger::DEBUG, "adding interpolation preprocess");
    p_->preprocesses.push_back(process);
}

}
