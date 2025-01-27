/*
  Fimex, src/coordSys/ObliqueMercatorProjection.cc

  Copyright (C) 2019 met.no

  Contact information:
  Norwegian Meteorological Institute
  Box 43 Blindern
  0313 OSLO
  NORWAY
  email: diana@met.no

  Project Info:  https://wiki.met.no/fimex/start

  This library is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  This library is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
  USA.
*/

/*
 * Fimex, ObliqueMercatorProjection.cc
 *
 * (C) Copyright 2017, SMHI
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
 *  Created on: May 12, 2017
 *      Author: Martin Raspaud
 */

#include "fimex/coordSys/ObliqueMercatorProjection.h"

#include "fimex/Logger.h"
#include "fimex/String2Type.h"

#include <regex>

#ifndef ACCEPT_USE_OF_DEPRECATED_PROJ_API_H
#define ACCEPT_USE_OF_DEPRECATED_PROJ_API_H
#endif

#include <proj_api.h>

namespace MetNoFimex {
using namespace std;

static Logger_p logger = getLogger("fimex.ObliqueMercatorProjection");

ObliqueMercatorProjection::ObliqueMercatorProjection()
    : ProjectionImpl("oblique_mercator", false)
{
}

ObliqueMercatorProjection::~ObliqueMercatorProjection() {}

bool ObliqueMercatorProjection::acceptsProj4(const std::string& proj4Str)
{
    return proj4ProjectionMatchesName(proj4Str, "omerc");
}

std::vector<CDMAttribute> ObliqueMercatorProjection::parametersFromProj4(const std::string& proj4Str)
{
    vector<CDMAttribute> attrs;
    if (!acceptsProj4(proj4Str)) return attrs;

    attrs.push_back(CDMAttribute("grid_mapping_name", "oblique_mercator"));

    // longitude at origin
    double longOfProjOrigin = 0.;
    std::smatch what;
    if (std::regex_search(proj4Str, what, std::regex("\\+lonc=(\\S+)"))) {
        longOfProjOrigin = string2type<double>(what[1].str());
    }
    attrs.push_back(CDMAttribute("longitude_of_projection_origin", longOfProjOrigin));

    // standard_parallel or scale_factor
    if (std::regex_search(proj4Str, what, std::regex("\\+lat_0=(\\S+)"))) {
        double latOfProjOrigin = string2type<double>(what[1].str());
        attrs.push_back(CDMAttribute("latitude_of_projection_origin", latOfProjOrigin));
    }

    // azimuth of central line
    double azimuthOfCentralLine = 0.;
    if (std::regex_search(proj4Str, what, std::regex("\\+alpha=(\\S+)"))) {
        azimuthOfCentralLine = string2type<double>(what[1].str());
    }
    attrs.push_back(CDMAttribute("azimuth_of_central_line", azimuthOfCentralLine));

    // gamma
    double gamma = 0.;
    if (std::regex_search(proj4Str, what, std::regex("\\+gamma=(\\S+)"))) {
        gamma = string2type<double>(what[1].str());
    }
    attrs.push_back(CDMAttribute("gamma", gamma));

    if (std::regex_search(proj4Str, what, std::regex("\\+k=(\\S+)"))) {
        attrs.push_back(CDMAttribute("scale_factor_at_projection_origin", string2type<double>(what[1].str())));
    }

    if (std::regex_search(proj4Str, what, std::regex("\\+no_rot"))) {
        attrs.push_back(CDMAttribute("no_rotation", ""));
    }

    proj4GetEarthAttributes(proj4Str, attrs);
    attrs.push_back(CDMAttribute("proj4", proj4Str));
    return attrs;
}

std::ostream& ObliqueMercatorProjection::getProj4ProjectionPart(std::ostream& oproj) const
{
    oproj << "+proj=omerc";
    addParameterToStream(oproj, "longitude_of_projection_origin", " +lonc=");
    addParameterToStream(oproj, "latitude_of_projection_origin", " +lat_0=");
    addParameterToStream(oproj, "azimuth_of_central_line", " +alpha=");
    addParameterToStream(oproj, "gamma", " +gamma=");
    addParameterToStream(oproj, "scale_factor_at_projection_origin", " +k_0=");
    addParameterToStream(oproj, "no_rotation", " +no_rot");
    return oproj;
}

} // namespace MetNoFimex
