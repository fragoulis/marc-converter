/* 
 * File:   MarConvertor.cpp
 * Author: fragoulis
 * 
 * Created on 24 Μάϊος 2013, 10:55 πμ
 */

#include "MarConvertor.h"
#include <fstream>
#include <iostream>
#include <ctime>
#include <stdlib.h>

#include <boost/xpressive/xpressive.hpp>
using namespace boost::xpressive;

using namespace json_spirit;

std::string type2str(const json_spirit::mValue &v) {
	switch (v.type()) {
		case json_spirit::obj_type: return "object";
			break;
		case json_spirit::array_type: return "array";
			break;
		case json_spirit::str_type: return "string";
			break;
		case json_spirit::bool_type: return "bool";
			break;
		case json_spirit::int_type: return "int";
			break;
		case json_spirit::real_type: return "real";
			break;
		case json_spirit::null_type: return "null";
			break;
	}
	return "null";
}

MarConvertor::MarConvertor()
: mCurrent(0)
, mRepeat(true) {
}

MarConvertor::~MarConvertor() {
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::clear() {
	FieldMap::iterator it = mUnimarcFields.begin();
	for (int i = 1; it != mUnimarcFields.end(); it++, i++) {
		delete it->second;
	}
	mUnimarcFields.clear();

	it = mMarc21Fields.begin();
	for (int i = 1; it != mMarc21Fields.end(); it++, i++) {
		delete it->second;
	}
	mMarc21Fields.clear();
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

int MarConvertor::convertMarc21ToUnimarc(char const *src, char const *dst, char const *rule) {

	std::ifstream rules;
	rules.open(rule);
	if (!rules.is_open()) {
		std::cerr << "ERROR: Unable to open rules file";
		return -1;
	}

	std::ofstream destStream;
	destStream.open(dst, std::ios::trunc | std::ios::binary);
	if (!destStream.is_open()) {
		std::cerr << "ERROR: Unable to open destination file";
		return -1;
	}

	mValue document;
	json_spirit::read(rules, document);
	mObject& root = document.get_obj();

	MARCBuffer<MARC21Record> marcBuffer;
	if (!marcBuffer.LoadFromFile(src)) {
		std::cerr << "ERROR: Unable to open source file";
		return -2;
	}

	MARC21Record marc21;
	while (marcBuffer.Read(marc21)) {

		// Map fields by  tag
		// The following loop reads the marc21 record and creates a map of 
		// key-value pairs where the key is the field's tag and the value
		// is a pointer to a vector of pointers to the fields.
		// That is because a record may and will contain repeatable fields
		// which we will want to access later to read their values.
		while (!marc21.IsEnd()) {
			const std::string& tagname = marc21.Tag()->c_str();
			FieldMap::iterator it = mMarc21Fields.find(tagname);
			if (it == mMarc21Fields.end()) {
				FieldVector *vec = new FieldVector;
				vec->reserve(3);
				vec->push_back(marc21.Field());
				mMarc21Fields.insert(FieldMap::value_type(tagname, vec));
			} else {
				it->second->push_back(marc21.Field());
			}
			marc21.MoveNext();
		}

		mArray& create_rules = root.at("create").get_array();
		createFields(create_rules);

		// Parse fields object description
		mObject& convert_rules = root.at("convert").get_obj();

		FieldMap::iterator marc21_it = mMarc21Fields.begin();
		for (; marc21_it != mMarc21Fields.end(); ++marc21_it) {
			FieldVector::iterator field_it = marc21_it->second->begin();
			for (; field_it != marc21_it->second->end(); ++field_it) {
				// 2. Convert field base on the rules
				convertField(convert_rules, *field_it);
			}
		}

		// 1. Convert leader
		UNIMARCRecord unimarc;
		FieldMap::iterator unimarc_it = mUnimarcFields.begin();
		for (; unimarc_it != mUnimarcFields.end(); unimarc_it++) {
			FieldVector* v = unimarc_it->second;
			FieldVector::iterator v_it = v->begin();
			for (; v_it != v->end(); v_it++) {
				unimarc.AddField(*v_it);
			}
		}

		writeRecord(destStream, unimarc, marc21.GetLeader().GetBuffer().StringPtr, root.at("leader").get_array());

		clear();
	}

	return 0;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::createFields(mArray &rules) {
	mArray::iterator rules_it = rules.begin();
	for (; rules_it != rules.end(); rules_it++) {
		mObject& field_desc = rules_it->get_obj();
		readAttributeField(field_desc);
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::convertField(mObject &rules, MARCField * src) {

	mCurrent = src;
	const std::string& src_tag = src->Tag().c_str();

	// Seach by tag name
	mObject::iterator field_rules_it = rules.find(src_tag);
	if (field_rules_it == rules.end()) {
		return;
	}

	// Get tag rules
	mArray& rules_a = field_rules_it->second.get_array();

	mArray::iterator rules_it = rules_a.begin();
	for (; rules_it != rules_a.end(); rules_it++) {
		mObject& rule = rules_it->get_obj();

		// Condition
		// Optional. The field is only created if condition is met
		mObject::iterator cond_it = rule.find("condition");
		if (cond_it != rule.end()) {
			mObject& cond_desc = cond_it->second.get_obj();
			if (!checkConditions(cond_desc, src)) {
				continue;
			}
		}

		// Clone
		mObject::iterator clone_it = rule.find("clone");
		if (clone_it != rule.end()) {
			if (clone_it->second.get_bool()) {
				pushField(src->Clone(), false);
				continue;
			}
		}

		// New field
		mObject::iterator field_it = rule.find("field");
		if (field_it != rule.end()) {
			mObject& field_desc = field_it->second.get_obj();
			readAttributeField(field_desc);
		}
	}

	mCurrent = 0;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

bool MarConvertor::checkConditions(mObject& desc, MARCField * src_field) {

	/**
	 * Rules
	 * <ul>
	 * <li>subfield_exists</li>
	 * <li>indicator1_not_eq</li>
	 * <li>indicator1_eq</li>
	 * <li>eq</li>
	 * <li>eq</li>
	 * </ul>
	 */

	static const std::string subfield_exists("subfield_exists")
			, indicator1_not_eq("indicator1_not_eq")
			, indicator1_eq("indicator1_eq")
			, eq("eq")
			;

	char *data = src_field->GetBuffer().StringPtr;

	mObject::iterator rule_it = desc.begin();
	for (; rule_it != desc.end(); ++rule_it) {
		const std::string& rule = rule_it->first;
		if (rule == subfield_exists) {
			char search[] = {
				UNIMARCRecord::SUBFIELD_INDICATOR,
				rule_it->second.get_str()[0],
				'\0'
			};

			// move data pointer @ the subfield data
			char*subfield_addr = strstr(data, search);

			if (!subfield_addr)
				return false;

		} else if (rule == indicator1_not_eq) {
			return rule_it->second.get_str()[0] != *data;
		} else if (rule == indicator1_eq) {
			return rule_it->second.get_str()[0] == *data;
		} else if (rule == eq) {

			std::string base;
			mArray& array_of_conditions = rule_it->second.get_array();
			mArray::iterator p_it = array_of_conditions.begin();
			for (; p_it != array_of_conditions.end(); ++p_it) {
				mObject& condition_obj = p_it->get_obj();
				std::string value = readValue(condition_obj);
				if (base.size() == 0)
					base = value;
				else if (base != value)
					return false;
			}
			return true;
		}
	}

	return true;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::pushField(MARCField* field, bool replace) {
	// Shorthand tag
	const std::string& tagName = field->Tag().c_str();

	if (field->GetBuffer().StringLength <= 3) {
		std::cerr << "Empty field '" << tagName << "'" << std::endl;
		return; // empty field
	}

	// Find fields of the same tag
	FieldMap::iterator it = mUnimarcFields.find(tagName);
	if (it == mUnimarcFields.end()) {
		// If not found, create a vector of fields for that tag
		FieldVector * fieldvector = new FieldVector();
		fieldvector->reserve(2);
		fieldvector->push_back(field);
		mUnimarcFields[tagName] = fieldvector;
	} else if (replace) {
		// If found, and is not repeatable we replace
		// It should only be one field
		FieldVector* fv = it->second;
		MARCField* old = fv->back();
		delete old;
		fv->clear();
		fv->push_back(field);
	} else {
		// If found we push
		it->second->push_back(field);
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::readAttributeField(mObject & field_desc) {
	MARCTag marcTag;

	// Get tag name
	mObject::iterator tag_it = field_desc.find("tag");
	if (tag_it == field_desc.end())
		throw std::runtime_error("Error: Field is missing 'tag' attribute");

	const std::string &tag = tag_it->second.get_str();
	marcTag.Parse(tag.c_str(), tag.size());

	if (marcTag.IsControl())
		throw std::runtime_error("Can't handle control field.");

	extractSubfields(mCurrent, mCurrentSubfields);

	mRepeat = false;
	do {
		// Is repeatable
		bool isFieldRepeatable = true;
		mObject::iterator repeat_it = field_desc.find("repeat");
		if (repeat_it != field_desc.end()) {
			isFieldRepeatable = repeat_it->second.get_bool();
		}

		bool addIndicators = true;
		ostringstream so;
		if (!isFieldRepeatable) {
			// Search for a field with the same tag.
			FieldMap::iterator same_it = mUnimarcFields.find(tag);
			if (same_it != mUnimarcFields.end()) {
				MARCField* old = same_it->second->back();
				so.write(old->GetString().StringPtr, old->GetString().StringLength);
				addIndicators = false;
			}
		}

		if (addIndicators) {
			char indicators[] = {' ', ' ', '\0'};
			readIndicators(field_desc, indicators);
			so.seekp(0, ios_base::beg);
			so << indicators;
		}

		readSubfields(field_desc, so);

		so.seekp(0, ios_base::end);
		so.put(UNIMARCRecord::END_OF_FIELD);

		MARCField * newField;
		if (marcTag.IsControl())
			newField = new MARCControlField;
		else
			newField = new MARCDataField;

		std::string stream = so.str();
		newField->Parse(marcTag, const_cast<char*> (stream.c_str()), stream.size());
		pushField(newField, !isFieldRepeatable);

	} while (mRepeat);

	mCurrentSubfields.clear();
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::readIndicators(mObject& desc, char* so) {
	mObject::iterator ind_it = desc.find("indicator1");
	if (ind_it != desc.end()) {
		readFieldIndicator(ind_it->second.get_obj(), so[0]);
	} else {
		so[0] = ' ';
	}
	ind_it = desc.find("indicator2");
	if (ind_it != desc.end()) {
		readFieldIndicator(ind_it->second.get_obj(), so[1]);
	} else {
		so[1] = ' ';
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::extractSubfields(MARCField* field, StringVecMap& subfields) {

	if (!field)
		return;

	MARC_String buffer = field->GetBuffer();

	if (!buffer.StringPtr) {
		std::cerr << "Buffer for field " << field->Tag().c_str() << " is null" << std::endl;
		return;
	}

	char *src = strchr(buffer.StringPtr, UNIMARCRecord::SUBFIELD_INDICATOR);
	if (!src) {
		return;
	}

	do {
		src++;
		std::string name(src, 1);
		src++;

		char *next = strchr(src, UNIMARCRecord::SUBFIELD_INDICATOR);
		if (!next)
			next = strchr(src, UNIMARCRecord::END_OF_FIELD);
		if (!next)
			break;

		std::string value(src, (int) next - (int) src);
		src = next;

		StringVecMap::iterator name_it = subfields.find(name);
		if (name_it == subfields.end())
			name_it = subfields.insert(StringVecMap::value_type(name, StringVec())).first;
		name_it->second.push_back(value);

	} while (1);
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::readSubfields(mObject& desc, ostringstream& so) {
	mArray& subfields = desc.at("subfields").get_array();
	mArray::iterator subfields_it = subfields.begin();
	for (; subfields_it != subfields.end(); subfields_it++) {
		mObject& subfield_desc = subfields_it->get_obj();
		readSubfield(subfield_desc, so);
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::readSubfield(mObject& desc, ostringstream& so) {

	// Optional. Value mapping
	mObject* map = NULL;
	mObject::iterator map_it = desc.find("map");
	if (map_it != desc.end()) {
		map = &map_it->second.get_obj();
	}

	bool isSubfieldRepeatable = true;
	mObject::iterator repeat_it = desc.find("repeat");
	if (repeat_it != desc.end()) {
		isSubfieldRepeatable = repeat_it->second.get_bool();
	}

	// Optional: Read size/offset
	// Must exist together. Otherwise ignored
	int length = 0, offset = 0;
	mObject::iterator size_it = desc.find("length");
	if (size_it != desc.end()) {
		mObject::iterator offset_it = desc.find("offset");
		if (offset_it != desc.end()) {
			length = size_it->second.get_int();
			offset = offset_it->second.get_int();
		}
	}

	// Copy subfield name
	mObject::iterator name_it = desc.find("name");
	if (name_it == desc.end())
		throw std::runtime_error("Error: Subfield is missing 'name' attribute");

	const std::string& name = name_it->second.get_str();

	std::string value = readValue(desc);

	if (value.empty())
		return;

	size_t subfield_pos = subfieldPos(so.str(), name);
	if (isSubfieldRepeatable || subfield_pos == std::string::npos) {

		// If field is repeatable or subfield does not exist
		// Create a new subfield and append it to the field
		so.put(UNIMARCRecord::SUBFIELD_INDICATOR);
		so.put(name[0]);
	} else {
		// If field is not repeatable and subfield exists
		// Apply the value on the existing subfield value
		// Move the stream pointer to the start of the subfield
		so.seekp(subfield_pos + 2, ios_base::beg);
	}

	// Filter value
	mObject::iterator filter_it = desc.find("filter");
	if (filter_it != desc.end()) {
		applyFilters(filter_it->second.get_obj(), value);
	}

	writeSubfieldValue(value, so, offset, length, map);
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::readValues(mObject &desc, StringVec & values) {
	// Either Value or Reference must exist
	mObject::iterator value_it = desc.find("value");
	if (value_it != desc.end()) {
		//value = readSubfieldValue(value_it->second.get_str());
		values.push_back(value_it->second.get_str());
	} else {
		value_it = desc.find("ref");
		if (value_it != desc.end()) {
			readSubfieldRefs(value_it->second.get_str(), values);
		} else {
			value_it = desc.find("expr");
			if (value_it != desc.end()) {
				values.push_back(readSubfieldRegex(value_it->second.get_str()));
			} else {
				value_it = desc.find("date");
				if (value_it != desc.end()) {
					values.push_back(readSubfieldDate(value_it->second.get_str()));
				} else {
					std::ostringstream os;
					std::cerr << "Parsing Error: Either 'value', 'ref', 'expr' or 'date' attribute must exist for subfield $";
					throw std::runtime_error(os.str());
				}
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

std::string MarConvertor::readValue(mObject & desc) {

	// Either Value or Reference must exist
	mObject::iterator value_it = desc.find("value");
	if (value_it != desc.end()) {
		//value = readSubfieldValue(value_it->second.get_str());
		return value_it->second.get_str();
	} else {
		value_it = desc.find("ref");
		if (value_it != desc.end()) {
			mObject::iterator pop_it = desc.find("pop");
			bool pop = (pop_it == desc.end()) || pop_it->second.get_bool();
			return readSubfieldRef(value_it->second.get_str(), pop);
		} else {
			value_it = desc.find("expr");
			if (value_it != desc.end()) {
				return readSubfieldRegex(value_it->second.get_str());
			} else {
				value_it = desc.find("date");
				if (value_it != desc.end()) {
					return readSubfieldDate(value_it->second.get_str());
				} else {
					std::ostringstream os;
					std::cerr << "Parsing Error: Either 'value', 'ref', 'expr' or 'date' attribute must exist for subfield $";
					throw std::runtime_error(os.str());
				}
			}
		}
	}

	return "";
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::applyFilters(mObject &filters, std::string & value) {

	/**
	 * Available filters:
	 * split: delim[index]|trim 
	 */

	mObject::iterator it = filters.begin();
	for (; it != filters.end(); ++it) {
		if (it->first == "split") {
			applyFilterSplit(it->second.get_str(), value);
		} else if (it->first == "trimright") {
			applyFilterTrimRight(it->second.get_str(), value);
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::applyFilterSplit(const std::string &filter, std::string & value) {

	sregex expr = sregex::compile("(.)\\[(\\d+)\\](\\|(\\w+))*");
	smatch m;
	if (regex_match(filter, m, expr)) {
		// Get reference value

		const std::string& delim = m[1].str();
		const int index = atoi(m[2].str().c_str());
		const std::string& filter = m[4].str();

		std::istringstream iss(value);
		std::vector<std::string> tokens;
		std::stringstream ss(value);
		std::string item;
		int cnt = 0;
		while (std::getline(ss, item, delim[0])) {
			tokens.push_back(item);
			if (index == cnt++)
				break;
		}

		if (index < tokens.size()) {
			value = tokens[index];
		}

	} else {
		std::ostringstream os;
		os << "Failed to match Filter Split regex '" << filter << "' on: " << value;
		throw std::runtime_error(os.str());
	}

}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::applyFilterTrimRight(const std::string &filter, std::string & value) {
	size_t pos = value.find_last_of(filter);
	if (pos != std::string::npos) {
		value.erase(pos);
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

std::string MarConvertor::readSubfieldValue(const std::string & original_value) {
	return original_value;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

std::string MarConvertor::readSubfieldRef(const std::string & original_value, bool pop) {
	// Parse ref value
	// Regex structure: tag + (opt) desc + (opt) [offset, len]
	// \d{3}(\$\w)?(\[\d+,\d+\])?
	// Match index: tag(1), desc(3), offset(5), length(6)

	sregex expr = sregex::compile("(\\d{3})?(\\$(\\w))?(\\[(\\d+),(\\d+)\\])?");
	smatch m;
	if (regex_match(original_value, m, expr)) {
		// Get reference value
		return findRefValue(m[1].str(), m[3].str(), m[5].str(), m[6].str(), pop);
	} else {
		std::ostringstream os;
		os << "Failed to match REF regex on: " << original_value;
		throw std::runtime_error(os.str());
	}

	return ""; // for compilation reasons
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::readSubfieldRefs(const std::string & original_value, StringVec & values) {
	// Parse ref value
	// Regex structure: tag + (opt) subfield + (opt) [offset, len]
	// \d{3}(\$\w)?(\[\d+,\d+\])?
	// Match index: tag(1), subfield(3), offset(5), length(6)

	sregex expr = sregex::compile("(\\d{3})?(\\$(\\w))?(\\[(\\d+),(\\d+)\\])?");
	smatch m;
	if (regex_match(original_value, m, expr)) {
		// Get reference value
		findRefValues(m[1].str(), m[3].str(), m[5].str(), m[6].str(), values);
	} else {
		std::ostringstream os;
		os << "Failed to match REF regex on: " << original_value;
		throw std::runtime_error(os.str());
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

std::string MarConvertor::readSubfieldRegex(const std::string & original_value) {

	sregex expr = sregex::compile("\\{(.*?)\\}");

	int const subs[] = {0, 1};
	sregex_token_iterator cur(original_value.begin(), original_value.end(), expr, subs);
	sregex_token_iterator end;

	std::vector<std::string> values;
	values.reserve(5);
	std::vector<std::string> matches;
	matches.reserve(5);

	// Read the referenced values
	for (int i = 0; cur != end; ++cur, ++i) {
		if (i % 2 == 0) {
			matches.push_back(*cur);
		} else {
			values.push_back(readSubfieldRef(*cur, false));
		}
	}

	size_t pos = 0;
	std::string value = original_value;
	for (int i = 0; i < matches.size(); ++i) {
		// find token
		pos = value.find(matches[i], pos);
		// replace token
		value.replace(pos, matches[i].size(), values[i]);
	}

	return value;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

std::string MarConvertor::readSubfieldDate(const std::string & original_value) {

	// current date/time based on current system
	time_t now = time(0);
	tm *ltm = localtime(&now);

	std::vector<std::string> tokens;
	tokens.reserve(4);
	tokens.push_back("yyyy");
	tokens.push_back("mm");
	tokens.push_back("dd");

	std::stringstream with[3];
	with[0] << 1900 + ltm->tm_year;
	with[1] << std::setfill('0') << std::setw(2) << 1 + ltm->tm_mon;
	with[2] << std::setfill('0') << std::setw(2) << ltm->tm_mday;

	size_t pos = 0;
	std::string value = original_value;
	for (int i = 0; i < tokens.size(); ++i) {
		const std::string& replace = with[i].str();
		const std::string& token = tokens[i];
		pos = value.find(token, pos);
		value.replace(pos, token.size(), replace);
	}

	return value;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::writeSubfieldValue(std::string &value, ostringstream& so, int offset, int length, mObject * map) {
	if (map) {
		mObject::iterator map_it = map->find(value);
		if (map_it == map->end()) {
			if (value == " ") {
				std::cerr << "Subfield map mismatched key: '" << value << "' {" << so.str() << "} for field '" << mCurrent->Tag().c_str() << "'" << std::endl;
				return;
			} else {
				std::ostringstream os;
				os << "Error: Subfield map mismatched key: '" << value << "' {" << so.str() << "} for field '" << mCurrent->Tag().c_str() << "'";
				throw std::runtime_error(os.str());
			}
		}
		value = map_it->second.get_str();
	}

	// Save the current stream size
	size_t current = so.tellp();
	so.seekp(0, ios_base::end);
	size_t streamSize = so.tellp();
	so.seekp(current);

	length = (length > 0 && length > value.size()) ? length : value.size();
	// if current position plus the offset
	if (current + offset + length > streamSize) {
		// stretch buffer to accomodate the new value
		//		so << std::setfill(' ') << std::setw(offset + length) << std::flush;
		for (int i = 0; i < offset + length; ++i)
			so.put(' ');
	}

	// 4 bytes indi
	so.seekp(current + offset, ios_base::beg);
	so.write(value.c_str(), value.size());
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

std::string MarConvertor::findRefValue(const std::string& tag, const std::string& subfield, const std::string& offset, const std::string & len, bool pop) {

	int iLen = 0, iOffset = 0;

	if (len.size() > 0) {
		iLen = atoi(len.c_str());
		iOffset = atoi(offset.c_str());
	}

	MARCField * field;
	if (tag.size() > 0) {
		field = getMarc21Field(tag, 0);

		if (!field) {
			std::cout << "Tag '" << tag << "' is missing. Cannot get referenced value" << std::endl;
			return "";
		}

	} else {
		field = mCurrent;
	}

	char *data = field->GetBuffer().StringPtr;
	char *start = data;

	if (subfield.size() > 0) {
		if (tag.size() > 0) {

			char needle[] = {
				UNIMARCRecord::SUBFIELD_INDICATOR,
				subfield[0],
				'\0'
			};

			StringVec values;
			while ((start = extractSubfieldValue(needle, start, iOffset, iLen, values)));

			std::cout << "Subfields appears " << values.size() << " times." << std::endl;
			return values[0]; // Return first occurence, regardless of how many times the subfield appears

		} else {
			StringVecMap::iterator it = mCurrentSubfields.find(subfield);
			if (it == mCurrentSubfields.end())
				return "";

			StringVec& subfield_values = it->second;
			if (subfield_values.empty())
				return "";

			std::string ret = subfield_values.back();
			if (pop)
				subfield_values.pop_back();
			mRepeat = pop && subfield_values.size() > 0;
			return ret;
		}

	} else if (iLen == 0) {
		char * end = strchr(start, UNIMARCRecord::END_OF_FIELD);
		if (!end)
			throw std::runtime_error("Cannot find end of field");
		iLen = end - start;
	}

	// staring from base + offset, copy len chars into ret.
	return std::string(start + iOffset, iLen);
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::findRefValues(const std::string& tag, const std::string& subfield, const std::string& offset, const std::string & len, StringVec & values) {

	int iLen = 0, iOffset = 0;
	if (len.size() > 0) {
		iLen = atoi(len.c_str());
		iOffset = atoi(offset.c_str());
	}

	MARCField * field;
	if (tag.size() > 0) {
		field = getMarc21Field(tag, 0);
		if (!field) {
			std::cout << "Tag '" << tag << "' is missing. Cannot get referenced value" << std::endl;
			return;
		}
	} else {
		field = mCurrent;
	}

	char *haystack = field->GetBuffer().StringPtr;
	char *start = haystack;

	if (subfield.size() > 0) {

		char needle[] = {
			UNIMARCRecord::SUBFIELD_INDICATOR,
			subfield[0],
			'\0'
		};

		while ((start = extractSubfieldValue(needle, start, iOffset, iLen, values))) {

		}

		return;

	} else if (iLen == 0) {
		// Calculate field size if no subfields are present
		char * end = strchr(start, UNIMARCRecord::END_OF_FIELD);
		if (!end) {
			std::ostringstream os;
			os << "Cannot find end of field";
			throw std::runtime_error(os.str());
		}
		iLen = end - start;
	}

	// staring from base + offset, copy len chars into ret.
	char ret[iLen + 1];
	::memcpy(ret, start + iOffset, iLen);
	ret[iLen] = '\0';
	values.push_back(ret);
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

char* MarConvertor::extractSubfieldValue(char* needle, char* start, int offset, int length, StringVec & values) {
	start = strstr(start, needle);

	if (!start)
		return 0;

	start += 2;

	if (length == 0) {
		// get pointer of next subfield or end of field
		char *end = strchr(start, UNIMARCRecord::SUBFIELD_INDICATOR);
		if (!end)
			end = strchr(start, UNIMARCRecord::END_OF_FIELD);
		if (!end)
			throw std::runtime_error("Cannot find end of subfield or field");
		length = end - start;
	}

	// staring from base + offset, copy len chars into ret.
	char ret[length + 1];
	::memcpy(ret, start + offset, length);
	ret[length] = '\0';
	values.push_back(ret);

	start += length;
	return start;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::readFieldIndicator(mObject& indicator, char &so) {
	char value;

	mObject::iterator v_it = indicator.find("value");
	if (v_it != indicator.end()) {
		value = v_it->second.get_str()[0];
	} else {
		v_it = indicator.find("ref");
		if (v_it != indicator.end()) {
			value = readIndicatorRef(v_it->second.get_str());
		} else {
			std::ostringstream os;
			std::cerr << "Parsing Error: Either 'value' or 'ref' attribute must exist for indicator i";
			throw std::runtime_error(os.str());
		}
	}

	mObject::iterator map = indicator.find("map");
	if (map != indicator.end()) {
		mObject& obj = map->second.get_obj();
		std::string key;
		key += value;

		mObject::iterator obj_it = obj.find(key);
		if (obj_it == obj.end()) {
			std::ostringstream os;
			std::cerr << "Error: Indicator map mismatched key: " << key;
			throw std::runtime_error(os.str());
		}
		value = obj_it->second.get_str()[0];
	}

	so = value;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

char MarConvertor::readIndicatorRef(const std::string & original_value) {
	// Parse ref value
	// \d{3}\i.{1}

	sregex expr = sregex::compile("(\\d{3})?\\i(.{1})");
	smatch m;
	if (regex_match(original_value, m, expr)) {
		// Get reference value
		return findRefIndicator(m[1].str(), m[2].str());
	} else {
		std::ostringstream os;
		os << "Failed to match REF regex on: " << original_value;
		throw std::runtime_error(os.str());
	}

	return '\0'; // for compilation reasons
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

size_t MarConvertor::subfieldPos(const std::string& data, const std::string& name, size_t pos) {
	char search[] = {
		UNIMARCRecord::SUBFIELD_INDICATOR,
		name[0],
	};
	return data.find(search, pos, 2);
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

char MarConvertor::findRefIndicator(const std::string& tag, const std::string & indicator) {
	MARCField * field;
	if (tag.size() > 0) {
		field = getMarc21Field(tag, 0);
	} else {
		field = mCurrent;
	}

	int iIndicator = atoi(indicator.c_str());
	char *data = field->GetBuffer().StringPtr;
	char ret = *(data + iIndicator - 1);
	return ret;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

void MarConvertor::convertLeader(mArray &desc, const char* src, char *dst) {
	const int count = desc.size();
	for (int i = 0; i < count; ++i) {
		mObject& obj = desc.at(i).get_obj();
		int offset = obj.at("offset").get_int();
		int size = obj.at("length").get_int();

		mObject::iterator value_it = obj.find("value");
		if (value_it != obj.end()) {
			mValue& value = value_it->second;
			switch (value.type()) {
				case json_spirit::null_type:
					::memset(dst + offset, ' ', size);
					break;
				case json_spirit::str_type:
					::memcpy(dst + offset, value.get_str().c_str(), size);
					break;

				default:
					std::cerr << "Leader: Undefined value type: " << type2str(value.type()) << std::endl;
			}

			continue;
		}

		mObject::iterator map_it = obj.find("map");
		if (map_it != obj.end()) {
			mObject& map = map_it->second.get_obj();

			std::string key(src + offset);
			key.resize(size);

			mValue& value = map.at(key);
			::memcpy(dst + offset, value.get_str().c_str(), size);
			continue;
		}

		::memcpy(dst + offset, src + offset, size);
	}
}

MARCField * MarConvertor::getMarc21Field(const std::string &tag, int index) {
	FieldMap::iterator it = mMarc21Fields.find(tag);
	if (it == mMarc21Fields.end()) {
		return NULL;
	}
	return it->second->at(index);
}

MARCField * MarConvertor::getUnimarcField(const std::string &tag, int index) {
	FieldMap::iterator it = mUnimarcFields.find(tag);
	if (it == mUnimarcFields.end()) {
		return NULL;
	}
	return it->second->at(index);
}
