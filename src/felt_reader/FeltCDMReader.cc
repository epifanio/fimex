#include "FeltCDMReader.h"
#include "Utils.h"
#include "Felt_File_Error.h"
#include "Felt_Array.h"
#include "interpolation.h"
#include "CDMDataType.h"
#include "DataImpl.h"
#include "ReplaceStringTimeObject.h"
#include <boost/shared_ptr.hpp>
#include <boost/regex.hpp>
#include <boost/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xinclude.h>
#include <sstream>
#include <iostream>
#include <cassert>
#include <ctime>

namespace MetNoUtplukk
{

using namespace std;

static CDMAttribute createCDMAttribute(string name, string datatype, string value) throw(CDMException)
{
	std::string type(string2lowerCase(datatype));
	if (type == "float") {
		float f;
		std::stringstream str(value);
		str >> f;
		return CDMAttribute(name, f);
	} else if (type == "double") {
		double d;
		std::stringstream str(value); 
		str >> d;
		return CDMAttribute(name, d);		
	} else if (type == "int") {
		int i;
		std::stringstream str(value);
		str >> i;
		return CDMAttribute(name, i);
	} else if (type == "short") {
		short s;
		std::stringstream str(value);
		str >> s;
		return CDMAttribute(name, s);
	} else if (type == "char") {
		char c;
		std::stringstream str(value);
		str >> c;
		return CDMAttribute(name, c);
	} else if (type == "string") {
		return CDMAttribute(name, value);
	} else {
		throw CDMException("unknown type: " + type);
	}
}

static string replaceTemplateAttribute(string value, const map<string, boost::shared_ptr<ReplaceStringObject> > templateReplacements) {
	for (map<string, boost::shared_ptr<ReplaceStringObject> >::const_iterator it = templateReplacements.begin(); it != templateReplacements.end(); ++it) {
		boost::smatch matches;
		boost::regex rgx(boost::regex(".*%" + it->first + "(\\((.*)\\))?" + "%.*"));
		if (boost::regex_match(value, matches, rgx)) {
			boost::shared_ptr<ReplaceStringObject> rso = it->second;
			if (matches.size() > 2) {
				// values within the inner brackets
				rso->setFormatString(matches[2]);
			}
			stringstream ss;
			rso->put(ss);
			value = boost::regex_replace(value, rgx, ss.str());
		}
	}
	return value;
}

static void fillAttributeList(vector<CDMAttribute>& attributes, xmlNodePtr node, const std::map<std::string, boost::shared_ptr<ReplaceStringObject> >& templateReplacements) {
	if (node == 0) return;
	string attribute("attribute");
	if ((node->type == XML_ELEMENT_NODE) &&
		(attribute == reinterpret_cast<const char *>(node->name))) {
			string name(reinterpret_cast<char *>(xmlGetProp(node, reinterpret_cast<const xmlChar *>("name"))));
			string value(reinterpret_cast<char *>(xmlGetProp(node, reinterpret_cast<const xmlChar *>("value"))));
			string type(reinterpret_cast<char *>(xmlGetProp(node, reinterpret_cast<const xmlChar *>("type"))));

			value = replaceTemplateAttribute(value, templateReplacements);
			attributes.push_back(createCDMAttribute(name,type,value));
	}
	fillAttributeList(attributes, node->children, templateReplacements);
	fillAttributeList(attributes, node->next, templateReplacements);
}

static int readXPathNode(boost::shared_ptr<xmlXPathContext>& xpathCtx, string& xpathString, std::map<string, string>& xmlAttributes, std::vector<CDMAttribute>& varAttributes, const map<string, boost::shared_ptr<ReplaceStringObject> > templateReplacements) throw(CDMException)
{
	boost::shared_ptr<xmlXPathObject> xpathObj(xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>(xpathString.c_str()), xpathCtx.get()), xmlXPathFreeObject);
	if (xpathObj.get() == 0) {
		throw CDMException("unable to parse xpath" + xpathString);
	}
	xmlNodeSetPtr nodes = xpathObj->nodesetval;
	int size = (nodes) ? nodes->nodeNr : 0; 
	if (size == 0) return 0;
	// only parsing node[0]
	xmlNodePtr node = nodes->nodeTab[0];
	if (node->type != XML_ELEMENT_NODE) {
		throw CDMException("xpath does not point to XML_ELEMENT_NODE: " + xpathString);
	}
	xmlAttrPtr attr = node->properties;
	while (attr != 0) {
		string name(reinterpret_cast<const char *>(attr->name));
		string value(reinterpret_cast<const char *>(attr->children->content));
		xmlAttributes[name] = value;
		attr = attr->next;
	}
	fillAttributeList(varAttributes, node->children, templateReplacements);
	return size;
}

// simple RAII for xmlCleanup
static void myXmlCleanupParser(int* i) {
	//std::cerr << "running xmlCleanupParser code" << std::endl;
	xmlCleanupParser();
	delete i;
}

FeltCDMReader::FeltCDMReader(std::string filename, std::string configFilename) throw (CDMException)
: filename(filename), configFilename(configFilename)
{
	try {
		init();
	} catch (MetNoFelt::Felt_File_Error& ffe) {
		throw CDMException(std::string("Felt_File_Error: ") + ffe.what());
	}
}

FeltCDMReader::~FeltCDMReader()
{
}

void FeltCDMReader::init() throw(MetNoFelt::Felt_File_Error) {
    // test lib vs compile version
	xmlInitParser();
    LIBXML_TEST_VERSION
    boost::shared_ptr<int> xmlCleanupRAII(new int(1), myXmlCleanupParser);
    boost::shared_ptr<xmlDoc> doc(xmlReadFile(configFilename.c_str(), NULL, 0), xmlFreeDoc);
    if (doc.get() == 0) {
    	throw MetNoFelt::Felt_File_Error("unable to parse config file: " + configFilename);
    }
    // apply the XInclude process
    if (xmlXIncludeProcess(doc.get()) < 0) {
        throw MetNoFelt::Felt_File_Error("XInclude processing failed\n");
    }
    
	boost::shared_ptr<xmlXPathContext> xpathCtx(xmlXPathNewContext(doc.get()), xmlXPathFreeContext);
	if (xpathCtx.get() == 0) {
		throw MetNoFelt::Felt_File_Error("unable to generate xpath context");
	}
	
	// open the feltFile with the desired parameters
	std::vector<std::string> knownFeltIds;
	{
		std::string xpathString("/cdm_felt_config/variables/parameter");
		boost::shared_ptr<xmlXPathObject> xpathObj(xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>(xpathString.c_str()), xpathCtx.get()), xmlXPathFreeObject);
		if (xpathObj.get() == 0) {
			throw MetNoFelt::Felt_File_Error("unable to parse xpath" + xpathString);
		}
		xmlNodeSetPtr nodes = xpathObj->nodesetval;
		for (int i = 0; i < nodes->nodeNr; ++i) {
			xmlNodePtr node = nodes->nodeTab[i];
			xmlChar* idName = xmlGetProp(node, reinterpret_cast<const xmlChar*>("id"));
			string id(reinterpret_cast<char*>(idName));
			xmlFree(idName);
			xmlChar* typeName = xmlGetProp(node, reinterpret_cast<const xmlChar*>("type"));
			// get the datatype
			std::string dataType;
			if (typeName != 0) {
				dataType = string(reinterpret_cast<char*>(typeName));
				xmlFree(typeName);
				dataType = ":dataType=" + dataType;
			}
			// get the fill value
			std::string fillValue;
			std::string xpathString("/cdm_felt_config/variables/parameter[@id=\""+id+"\"]/attribute[@name=\"_FillValue\"]");
			boost::shared_ptr<xmlXPathObject> xpathObj(xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>(xpathString.c_str()), xpathCtx.get()), xmlXPathFreeObject);
			if (xpathObj.get() == 0) {
				throw MetNoFelt::Felt_File_Error("unable to parse xpath" + xpathString);
			}
			if (xpathObj->nodesetval->nodeNr > 0) {
				xmlChar* fillValName = xmlGetProp(xpathObj->nodesetval->nodeTab[0], reinterpret_cast<const xmlChar*>("value"));
				fillValue = ":fillValue=" + std::string(reinterpret_cast<char*>(fillValName));
				xmlFree(fillValName);
			}
			knownFeltIds.push_back(id + dataType + fillValue);
		}
	}
	feltFile = MetNoFelt::Felt_File(filename, knownFeltIds);
	{
		// fill templateReplacementAttributes: MIN_DATETIME, MAX_DATETIME
		std::vector<time_t> feltTimes = feltFile.getFeltTimes();
		templateReplacementAttributes["MIN_DATETIME"] = boost::shared_ptr<ReplaceStringObject>(new ReplaceStringTimeObject(feltTimes[0]));
		templateReplacementAttributes["MAX_DATETIME"] = boost::shared_ptr<ReplaceStringObject>(new ReplaceStringTimeObject(feltTimes[feltTimes.size() - 1]));
	}
	
	// fill the CDM;
	// set the global data for this feltFile derived from first data
	// TODO: translate producer-ids to something useful?
	
	// global attributes from config
	{
		std::string xpathString("/cdm_felt_config/global_attributes");
		boost::shared_ptr<xmlXPathObject> xpathObj(xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>(xpathString.c_str()), xpathCtx.get()), xmlXPathFreeObject);
		if (xpathObj.get() == 0) {
			throw MetNoFelt::Felt_File_Error("unable to parse xpath" + xpathString);
		}
		xmlNodeSetPtr nodes = xpathObj->nodesetval;
		if (nodes->nodeNr != 1) {
			throw MetNoFelt::Felt_File_Error("unable to find " + xpathString + " in config: " + configFilename);
		}
		for (int i = 0; i < nodes->nodeNr; ++i) {
			xmlNodePtr node = nodes->nodeTab[i];
			assert(node->type == XML_ELEMENT_NODE);
			std::vector<CDMAttribute> globAttributes;
			fillAttributeList(globAttributes, nodes->nodeTab[0]->children, templateReplacementAttributes);
			for (std::vector<CDMAttribute>::iterator it = globAttributes.begin(); it != globAttributes.end(); ++it) {
				cdm.addAttribute(cdm.globalAttributeNS(), *it);
			}
		}
		if (! cdm.checkVariableAttribute(cdm.globalAttributeNS(), "history", boost::regex(".*"))) {
			tm my_tmtime;
			time_t mytime;
			time(&mytime);
			gmtime_r(&mytime, &my_tmtime);
			std::stringstream stime;
			int month = (my_tmtime.tm_mon + 1);
			stime << (my_tmtime.tm_year + 1900) << "-" <<  (month < 10 ? "0" : "") << month << "-" << ( my_tmtime.tm_mday < 10 ? "0" : "") << my_tmtime.tm_mday;
			//stime << (my_tmtime.tm_hour) << ":" << my_tmtime.tm_min << ":" << my_tmtime.tm_sec << "Z";
			cdm.addAttribute(cdm.globalAttributeNS(), CDMAttribute("history", stime.str() + " creation by utplukk from file '"+ filename+"'"));
		}
	}
	
	
	// add axes
	// time
	CDMDimension timeDim("null", 0);
	{
		std::string xpathTimeString("/cdm_felt_config/axes/time");
		boost::shared_ptr<xmlXPathObject> xpathObj(xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>(xpathTimeString.c_str()), xpathCtx.get()), xmlXPathFreeObject);
		if (xpathObj.get() == 0) {
			throw MetNoFelt::Felt_File_Error("unable to parse xpath" + xpathTimeString);
		}
		xmlNodeSetPtr nodes = xpathObj->nodesetval;
		if (nodes->nodeNr != 1) {
			throw MetNoFelt::Felt_File_Error("unable to find exactly 1 'time'-axis in config: " + configFilename);
		}
		xmlNodePtr node = nodes->nodeTab[0];
		assert(node->type == XML_ELEMENT_NODE);
		string timeName(reinterpret_cast<char *>(xmlGetProp(node, reinterpret_cast<const xmlChar *>("name"))));
		string timeType(reinterpret_cast<char *>(xmlGetProp(node, reinterpret_cast<const xmlChar *>("type"))));
		CDMDataType timeDataType = string2datatype(timeType);
		long timeSize = feltFile.getFeltTimes().size();
		timeDim = CDMDimension(timeName, timeSize);
		timeDim.setUnlimited(true);
		cdm.addDimension(timeDim);
		std::vector<std::string> timeShape;
		timeShape.push_back(timeDim.getName());
		CDMVariable timeVar(timeName, timeDataType, timeShape);
		timeVec = feltFile.getFeltTimes();
		boost::shared_ptr<Data> timeData = createData(timeDataType, timeSize, timeVec.begin(), timeVec.end());
		timeVar.setData(timeData);
		cdm.addVariable(timeVar);
		std::vector<CDMAttribute> timeAttributes;
		fillAttributeList(timeAttributes, nodes->nodeTab[0]->children, templateReplacementAttributes);
		for (std::vector<CDMAttribute>::iterator it = timeAttributes.begin(); it != timeAttributes.end(); ++it) {
			cdm.addAttribute(timeVar.getName(), *it);
		}
	}
	
	// levels
	map<short, CDMDimension> levelDims;
	{
		std::map<short, std::vector<short> > levels = feltFile.getFeltLevels();
		for (std::map<short, std::vector<short> >::const_iterator it = levels.begin(); it != levels.end(); ++it) {
			// add a level
			std::string xpathLevelString("/cdm_felt_config/axes/vertical_axis[@felt_id='"+type2string(it->first)+"']");
			boost::shared_ptr<xmlXPathObject> xpathObj(xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>(xpathLevelString.c_str()), xpathCtx.get()), xmlXPathFreeObject);
			if (xpathObj.get() == 0) {
				throw MetNoFelt::Felt_File_Error("unable to parse xpath" + xpathLevelString);
			}
			xmlNodeSetPtr nodes = xpathObj->nodesetval;
			if (nodes->nodeNr != 1) {
				throw MetNoFelt::Felt_File_Error("unable to find 'vertical'-axis "+type2string(it->first)+" in config: " + configFilename);
			}
			xmlNodePtr node = nodes->nodeTab[0];
			assert(node->type == XML_ELEMENT_NODE);
			string levelName(reinterpret_cast<char *>(xmlGetProp(node, reinterpret_cast<const xmlChar *>("name"))));
			string levelId(reinterpret_cast<char *>(xmlGetProp(node, reinterpret_cast<const xmlChar *>("id"))));
			string levelType(reinterpret_cast<char *>(xmlGetProp(node, reinterpret_cast<const xmlChar *>("type"))));
			CDMDataType levelDataType = string2datatype(levelType);
			long levelSize = it->second.size();
			CDMDimension levelDim(levelId, levelSize);
			levelDims.insert(std::pair<short, CDMDimension>(it->first, levelDim));
			cdm.addDimension(levelDim);
			levelVecMap[levelDim.getName()] = it->second;

			std::vector<std::string> levelShape;
			levelShape.push_back(levelDim.getName());
			CDMVariable levelVar(levelId, levelDataType, levelShape);
			const std::vector<short>& lv = it->second;
			boost::shared_ptr<Data> data = createData(levelDataType, levelSize, lv.begin(), lv.end());
			levelVar.setData(data);
			cdm.addVariable(levelVar);
			
			std::vector<CDMAttribute> levelAttributes;
			fillAttributeList(levelAttributes, nodes->nodeTab[0]->children, templateReplacementAttributes);
			for (std::vector<CDMAttribute>::iterator it = levelAttributes.begin(); it != levelAttributes.end(); ++it) {
				cdm.addAttribute(levelVar.getName(), *it);
			}
		}
	}
    
	//x,y dim will be set with the projection, can also = long/lat
	// setting default-value
    xDim = CDMDimension("x", feltFile.getNX());
    yDim = CDMDimension("y", feltFile.getNY());
	
    // projection of the array (currently only one allowed
    std::string projName;
	std::string coordinates("");
    {
    	const boost::array<float, 6>& gridParams = feltFile.getGridParameters();
    	short gridType = feltFile.getGridType();
    	std::string projStr = MetNoFelt::getProjString(gridType, gridParams);
    	projName = std::string("projection_" + type2string(gridType));
    	// projection-variable without datatype and dimension
    	CDMVariable projVar(projName, CDM_NAT, std::vector<std::string>());
    	cdm.addVariable(projVar);
    	std::vector<CDMAttribute> projAttr = projStringToAttributes(projStr);
    	for (std::vector<CDMAttribute>::iterator attrIt = projAttr.begin(); attrIt != projAttr.end(); ++attrIt) {
    		cdm.addAttribute(projName, *attrIt);
    	}
    	
    	{
    	// create the x dimension variables and dimensions
			std::string xpathStringX("/cdm_felt_config/axes/spatial_axis[@projection_felt_id='"+type2string(gridType)+"' and @id='x']");
			std::vector<CDMAttribute> xVarAttributes;
			std::map<string, string> xXmlAttributes;
			int found = readXPathNode(xpathCtx, xpathStringX, xXmlAttributes, xVarAttributes, templateReplacementAttributes);
			if (found != 1) {
				throw MetNoFelt::Felt_File_Error("error in config-file: not exactly 1 entry for xpath: " + xpathStringX);
			}
			std::string xName(xXmlAttributes["name"]);
			xDim = CDMDimension(xName, feltFile.getNX());
			CDMDataType xDataType = string2datatype(xXmlAttributes["type"]);
			std::vector<std::string> xDimShape;
			xDimShape.push_back(xDim.getName());
			CDMVariable xVar(xName, xDataType, xDimShape);
			xVar.setData(feltFile.getXData());
			cdm.addDimension(xDim);
			cdm.addVariable(xVar);
			for (std::vector<CDMAttribute>::iterator attrIt = xVarAttributes.begin(); attrIt != xVarAttributes.end(); ++attrIt) {
				cdm.addAttribute(xName, *attrIt);
			}
    	}
    	{
    		// create the y dimension variables and dimensions
    		std::string xpathStringY("/cdm_felt_config/axes/spatial_axis[@projection_felt_id='"+type2string(gridType)+"' and @id='y']");
    		std::vector<CDMAttribute> yVarAttributes;
    		std::map<string, string> yXmlAttributes;
    		int found = readXPathNode(xpathCtx, xpathStringY, yXmlAttributes, yVarAttributes, templateReplacementAttributes);
    		if (found != 1) {
    			throw MetNoFelt::Felt_File_Error("error in config-file: not exactly 1 entry for xpath: " + xpathStringY);    		
    		}
    		std::string yName(yXmlAttributes["name"]);
    		yDim = CDMDimension(yName, feltFile.getNY());
    		CDMDataType yDataType = string2datatype(yXmlAttributes["type"]);
    		std::vector<std::string> yDimShape;
    		yDimShape.push_back(yDim.getName());
    		CDMVariable yVar(yName, yDataType, yDimShape);
    		yVar.setData(feltFile.getYData());
    		cdm.addDimension(yDim);
    		cdm.addVariable(yVar);
    		for (std::vector<CDMAttribute>::iterator attrIt = yVarAttributes.begin(); attrIt != yVarAttributes.end(); ++attrIt) {
    			cdm.addAttribute(yName, *attrIt);
    		}
    	}
    	
    	std::string longName;
    	std::string latName;
    	{
    		// read longitude and latitude names for projection axes
    		std::string xpathStringLong("/cdm_felt_config/axes/spatial_axis[@id='longitude']");
    		std::vector<CDMAttribute> lonlatVarAttributes;
    		std::map<string, string> lonlatXmlAttributes;
    		int found = readXPathNode(xpathCtx, xpathStringLong, lonlatXmlAttributes, lonlatVarAttributes, templateReplacementAttributes);
    		if (found != 1) {
    			throw MetNoFelt::Felt_File_Error("error in config-file: not exactly 1 entry for xpath: " + xpathStringLong);    		
    		}
    		longName = lonlatXmlAttributes["name"];
    		std::string xpathStringLat("/cdm_felt_config/axes/spatial_axis[@id='latitude']");
    		found = readXPathNode(xpathCtx, xpathStringLat, lonlatXmlAttributes, lonlatVarAttributes, templateReplacementAttributes);
    		if (found != 1) {
    			throw MetNoFelt::Felt_File_Error("error in config-file: not exactly 1 entry for xpath: " + xpathStringLat);    		
    		}
    		latName = lonlatXmlAttributes["name"]; 		
    	}
    	
    	// add projection axes 'coordinates = "lon lat";
    	if (xDim.getName() != longName && yDim.getName() != latName) {
			coordinates = longName + " " + latName;
			try {
				cdm.generateProjectionCoordinates(projName, xDim.getName(), yDim.getName(), longName, latName);
			} catch (MetNoUtplukk::CDMException& ex) {
				throw MetNoFelt::Felt_File_Error(ex.what());
			}
		}
    }

    // add variables
	std::vector<MetNoFelt::Felt_Array> fArrays(feltFile.listFeltArrays());
	for (std::vector<MetNoFelt::Felt_Array>::iterator it = fArrays.begin(); it != fArrays.end(); ++it) {
		std::string xpathString("/cdm_felt_config/variables/parameter[@id='"+it->getName()+"']");
		boost::shared_ptr<xmlXPathObject> xpathObj(xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>(xpathString.c_str()), xpathCtx.get()), xmlXPathFreeObject);
		if (xpathObj.get() == 0) {
			throw MetNoFelt::Felt_File_Error("unable to parse xpath" + xpathString);
		}
		xmlNodeSetPtr nodes = xpathObj->nodesetval;
		int size = (nodes) ? nodes->nodeNr : 0;
		if (size > 1) {
			throw MetNoFelt::Felt_File_Error("error in config-file: several entries for parameter: " + it->getName());
		}
    	if (size < 1) {
    		std::cerr << "config-file doesn't contain parameter: " << xpathString << std::endl;
    	} else {
    		assert(nodes->nodeTab[0]->type == XML_ELEMENT_NODE);
    		std::string varName(reinterpret_cast<char *>(xmlGetProp(nodes->nodeTab[0], reinterpret_cast<const xmlChar *>("name"))));
    		std::vector<CDMAttribute> attributes;
    		fillAttributeList(attributes, nodes->nodeTab[0]->children, templateReplacementAttributes);
    		// add the projection
    		attributes.push_back(CDMAttribute("grid_mapping",projName));
    		if (coordinates != "") {
    			attributes.push_back(CDMAttribute("coordinates", coordinates));
    		}
    	
    		// map shape, generate variable, set attributes/variable to CDM (fastest moving index (x) first, slowest (unlimited, time) last
    		std::vector<std::string> shape;
    		shape.push_back(xDim.getName());
    		shape.push_back(yDim.getName());
    		if (it->getLevels().size() > 1) {
    			shape.push_back(levelDims[it->getLevelType()].getName());
    		}
    		if (it->getTimes().size() > 0) {
    			shape.push_back(timeDim.getName());
    		}
    		CDMDataType type = string2datatype(it->getDatatype());
    		CDMVariable var(varName, type, shape);
    		cdm.addVariable(var);
    		varNameFeltIdMap[varName] = it->getName();
    		//  update scaling factor attribute with value from felt-file and xml-setup (only for short-values)
    		if (it->getScalingFactor() != 1) {
    			bool found = false;
        		for (std::vector<CDMAttribute>::iterator attrIt = attributes.begin(); attrIt != attributes.end(); ++attrIt) {
        			if (attrIt->getName() == "scale_factor") {
        				found = true;
        				float scale = (attrIt->getData()->asConstFloat())[0] * it->getScalingFactor();
        				attrIt->getData()->setValue(0, scale);
        			}
        		}
        		if (! found) {
        			attributes.push_back(CDMAttribute("scale_factor", static_cast<float>(it->getScalingFactor())));
        		}
    		}
    		for (std::vector<CDMAttribute>::const_iterator attrIt = attributes.begin(); attrIt != attributes.end(); ++attrIt) {
    			cdm.addAttribute(varName, *attrIt);
    		}
    	}
	}	
}


const boost::shared_ptr<Data> FeltCDMReader::getDataSlice(const std::string& varName, size_t unLimDimPos) throw(CDMException) {
	const CDMVariable& variable = cdm.getVariable(varName);
	if (variable.hasData()) {
		return getDataFromMemory(variable, unLimDimPos);
	}
	// only time can be unLimDim
	if (unLimDimPos > timeVec.size()) {
		throw CDMException("requested time outside data-region");
	}	
	
	// felt data can be x,y,level,time; x,y,level; x,y,time; x,y;
	const vector<std::string>& dims = variable.getShape();
	const CDMDimension* layerDim = 0;
	size_t xy_size = 1;
	for (vector<std::string>::const_iterator it = dims.begin(); it != dims.end(); ++it) {
		CDMDimension& dim = cdm.getDimension(*it);		
		if (dim.getName() != xDim.getName() &&
			dim.getName() != yDim.getName() &&
			!dim.isUnlimited())
		{
			layerDim = &dim;
		}
		if (! dim.isUnlimited()) {
			xy_size *= dim.getLength();
		}
	}
	boost::shared_ptr<Data> data = createData(variable.getDataType(), xy_size);
	try {
		std::map<std::string, std::string>::const_iterator foundId = varNameFeltIdMap.find(variable.getName()); 
		if (foundId != varNameFeltIdMap.end()) {
			MetNoFelt::Felt_Array& fa = feltFile.getFeltArray(foundId->second);

			// test for availability of the current time in the variable (getSlice will get data for every time)
			vector<long> faTimes = fa.getTimes();
			short contains = false;
			for (vector<long>::const_iterator it = faTimes.begin(); it != faTimes.end(); it++) {
				if (*it == timeVec[unLimDimPos]) {
					contains = true;
				}
			}
			if (!contains) {
				// return empty dataset
				return createData(variable.getDataType(), 0);
			}

			// select all available layers
			std::vector<short> layerVals;
			if ((layerDim != 0) && (layerDim->getLength() > 0)) {
				for (size_t i = 0; i < layerDim->getLength(); ++i) {
					layerVals.push_back(levelVecMap[layerDim->getName()][i]);
				}
			} else {
				// no layers, just 1 level
				std::vector<short> levels = fa.getLevels();
				if (levels.size() == 1) {
					layerVals.push_back(*(levels.begin()));
				} else {
					throw CDMException("variable " +variable.getName() + " has unspecified levels");
				}
			}
			size_t dataCurrentPos = 0;
			for (std::vector<short>::const_iterator lit = layerVals.begin(); lit != layerVals.end(); ++lit) {
				boost::shared_ptr<Data> levelData = feltFile.getScaledDataSlice(varNameFeltIdMap[variable.getName()], timeVec[unLimDimPos], *lit, fa.getFillValue());
				data->setValues(dataCurrentPos, *levelData, 0, levelData->size());
				dataCurrentPos += levelData->size();
			}
		}
	} catch (MetNoFelt::Felt_File_Error& ffe) {
		throw CDMException(std::string("Felt_File_Error: ") + ffe.what());
	}
	return data;
}

}
