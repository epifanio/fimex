#include "Null_CDMWriter.h"
#include <boost/shared_array.hpp>
#include "CDMDataType.h"
#include "Utils.h"

namespace MetNoUtplukk
{

static bool putRecData(CDMDataType dt, boost::shared_ptr<Data> data, size_t recNum) {
	if (data->size() == 0) return true;
	
	switch (dt) {
	case CDM_NAT: return false;
	case CDM_CHAR:
	case CDM_STRING: data->asChar().get(); break;
	case CDM_SHORT:  data->asShort().get(); break;
	case CDM_INT:    data->asInt().get(); break;
	case CDM_FLOAT:  data->asFloat().get(); break;
	case CDM_DOUBLE: data->asDouble().get(); break;
	default: return false;
	}
	return true;
	
}


static bool putVarData(CDMDataType dt, boost::shared_ptr<Data> data) {
	size_t size = data->size();
	if (size == 0) return true;
	
	int dims = 5;
	int dim_size = 1;
	for (int i = 0; i < dims; i++) {
		dim_size *= 3;
	}
	
	switch (dt) {
	case CDM_NAT: return false;
	case CDM_CHAR:
	case CDM_STRING: data->asChar().get(); break;
	case CDM_SHORT:  data->asShort().get(); break;
	case CDM_INT:    data->asInt().get(); break;
	case CDM_FLOAT:  data->asFloat().get(); break;
	case CDM_DOUBLE: data->asDouble().get(); break;
	default: return false;
	}
	return true;
}

Null_CDMWriter::Null_CDMWriter(const boost::shared_ptr<CDMReader> cdmReader, const std::string& outputFile)
: CDMWriter(cdmReader, outputFile)
{
	const CDM& cdm = cdmReader->getCDM();
	const CDM::StrDimMap& cdmDims = cdm.getDimensions();
	// define dims
	for (CDM::StrDimMap::const_iterator it = cdmDims.begin(); it != cdmDims.end(); ++it) {
		int length = it->second.isUnlimited() ? 0 : it->second.getLength();
		length++;
	}

	// define vars
	const CDM::StrVarMap& cdmVars = cdm.getVariables();
	for (CDM::StrVarMap::const_iterator it = cdmVars.begin(); it != cdmVars.end(); ++it) {
		const CDMVariable& var = it->second;
		const std::vector<std::string>& shape = var.getShape();
		for (size_t i = 0; i < shape.size(); i++) {
			// revert order, cdm requires fastest moving first, netcdf-cplusplus requires fastest moving first
		}
		CDMDataType datatype = var.getDataType();
		if (datatype == CDM_NAT && shape.size() == 0) {
			datatype = CDM_INT;
		}
	}
	
	// write attributes
	const CDM::StrStrAttrMap& cdmAttrs = cdm.getAttributes();
	// using C interface since it offers a combined interface to global and var attributes
	for (CDM::StrStrAttrMap::const_iterator it = cdmAttrs.begin(); it != cdmAttrs.end(); ++it) {
		int varId;
		if (it->first == CDM::globalAttributeNS()) {
			varId = 0;
		} else {
			varId = 1;
		}
		for (CDM::StrAttrMap::const_iterator ait = it->second.begin(); ait != it->second.end(); ++ait) {
			const CDMAttribute& attr = ait->second;
			CDMDataType dt = attr.getDataType();
			switch (dt) {
			case CDM_STRING: ;
			case CDM_CHAR:
				attr.getData()->asChar().get();
				break;
			case CDM_SHORT:
				attr.getData()->asShort().get();
				break;
			case CDM_INT:
				attr.getData()->asInt().get();
				break;
			case CDM_FLOAT:
				attr.getData()->asFloat().get();
				break;
			case CDM_DOUBLE:
				attr.getData()->asDouble().get();
				break;
			case CDM_NAT:
			default: throw CDMException("unknown datatype for attribute " + attr.getName());
			}
		}
	}
	
	// write data
	for (CDM::StrVarMap::const_iterator it = cdmVars.begin(); it != cdmVars.end(); ++it) {
		const CDMVariable& cdmVar = it->second;
		if (!cdm.hasUnlimitedDim(cdmVar)) {
			boost::shared_ptr<Data> data = cdmReader->getDataSlice(cdmVar.getName());
			if (!putVarData(cdmVar.getDataType(), data)) {
				throw CDMException("problems writing data to var " + cdmVar.getName() + ": " + ", datalength: " + type2string(data->size()));
			}
		} else {
			// iterate over each unlimited dim (usually time)
			const CDMDimension* unLimDim = cdm.getUnlimitedDim();
			for (size_t i = 0; i < unLimDim->getLength(); ++i) {
				boost::shared_ptr<Data> data = cdmReader->getDataSlice(cdmVar.getName(), i);
				if (!putRecData(cdmVar.getDataType(), data, i)) {
					throw CDMException("problems writing datarecord " + type2string(i) + " to var " + cdmVar.getName() + ": " + ", datalength: " + type2string(data->size()));
				}
			}

		}
	}
	
}

Null_CDMWriter::~Null_CDMWriter()
{
}

}
