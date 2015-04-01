/* 
 * File:   MarConvertor.h
 * Author: fragoulis
 *
 * Created on 24 Μάϊος 2013, 10:55 πμ
 */

#ifndef MARCCONVERT_H
#define	MARCCONVERT_H

#include <json_spirit.h>
#include <mtl.h>

class MarConvertor {
public:

	/**
	 * Converts a marc file from one formal to another based on a custom 
	 * rule file.
	 * 
	 * @param char const* src The source file.
	 * @param char const* dst The destination file. If it exists, it will be truncated.
	 * @param char const* rules The rule file.
	 */
	int convertMarc21ToUnimarc(char const*, char const*, char const*);

	MarConvertor();
	~MarConvertor();

private:

	//	typedef std::map<std::string, MARCField*> FieldPrtMap;
	typedef std::vector<MARCField*> FieldVector;
	typedef std::map<std::string, FieldVector*> FieldMap;
	typedef std::vector<std::string> StringVec;
	typedef std::vector<std::ostringstream> StreamVector;
	typedef std::map<std::string, std::string> StringMap;
	typedef std::map<std::string, StringVec> StringVecMap;

	FieldMap mMarc21Fields;
	FieldMap mUnimarcFields;

	MARCField* mCurrent;
	StringVecMap mCurrentSubfields;
	bool mRepeat;

	/**
	 * When reading rules and creating new fields every stream
	 * represents a field.
	 * When a method requires more than one field to be created 
	 * out of its values,
	 * it appends more streams.
	 */
	StreamVector mStreams;

	/**
	 * 
	 * @param 
	 * @param 
	 * @return 
	 */
	MARCField* getMarc21Field(const std::string &, int);

	/**
	 * 
	 * @param 
	 * @param 
	 * @return 
	 */
	MARCField* getUnimarcField(const std::string &, int);

	/**
	 * 
	 * @param root
	 * @param marc21
	 * @param unimarc
	 */
	void convertLeader(json_spirit::mArray &, char const*, char*);

	/**
	 * 
	 * @param 
	 */
	void createFields(json_spirit::mArray&);

	/**
	 * 
	 * @param 
	 * @param 
	 */
	void convertField(json_spirit::mObject&, MARCField*);

	/**
	 * 
	 * @param 
	 * @param 
	 */
	void pushField(MARCField*, bool);
	void readIndicators(json_spirit::mObject&, char*);
	std::string readSubfieldValue(const std::string&);
	std::string readSubfieldRef(const std::string&,bool);
	void readSubfieldRefs(const std::string&, StringVec&);
	std::string readSubfieldRegex(const std::string&);
	std::string readSubfieldDate(const std::string&);
	void writeSubfieldValue(std::string&, ostringstream&, int, int, json_spirit::mObject*);
	void readSubfield(json_spirit::mObject&, ostringstream&);
	void readSubfields(json_spirit::mObject&, ostringstream&);
	void readAttributeField(json_spirit::mObject&);
	std::string readValue(json_spirit::mObject&);
	void readValues(json_spirit::mObject&, StringVec&);

	char* extractSubfieldValue(char*, char*, int, int, StringVec &);
	void extractSubfields(MARCField*, StringVecMap&);
	void getSubfieldNames(const char*, int, StringVec&);

	bool checkConditions(json_spirit::mObject&, MARCField *);
	size_t subfieldPos(const std::string&, const std::string&, size_t = 0);

	void readFieldIndicator(json_spirit::mObject& indicator, char&);
	char readIndicatorRef(const std::string &);
	char findRefIndicator(const std::string&, const std::string&);
	std::string findRefValue(const std::string&, const std::string&, const std::string&, const std::string&,bool);
	void findRefValues(const std::string&, const std::string&, const std::string&, const std::string&, StringVec&);
	void applyFilters(json_spirit::mObject&, std::string&);
	void applyFilterSplit(const std::string&, std::string&);
	void applyFilterTrimRight(const std::string&, std::string&);

	void clear();

	template<class TLeader, class TDir >
	ostream & writeRecord(ostream &, MARCRecord<TLeader, TDir> &, char*, json_spirit::mArray&);
};

template<class TLeader, class TDir>
ostream& MarConvertor::writeRecord(ostream &os, MARCRecord<TLeader, TDir> &rec, char* marc21_leader, json_spirit::mArray& leader_desc) {

	// Vars
	std::vector<std::string> fields;
	std::vector<std::string> directory;
	long data_end = 0;

	rec.MoveFirst();
	while (!rec.IsEnd()) {

		// Get data in raw format
		std::string str = rec.Field()->GetBuffer();
		fields.push_back(str);

		// Create directory entry
		int len = str.size();
		char buff[13];
		sprintf(buff, "%03s%04d%05d", rec.Tag()->c_str(), len, data_end);
		std::string direntry(buff);
		directory.push_back(direntry);
		data_end += len;

		rec.MoveNext();
	}

	/**
	 * Rules from MARC::Record::USMARC
	 */
	long base_address
			= MARCRecord<TLeader, TDir>::LEADER_LEN + // better be 24
			(directory.size() * MARCRecord<TLeader, TDir>::DIRECTORY_ENTRY_LEN) +
			// all the directory entries
			1; // end-of-field marker


	long total
			= base_address + // stuff before first field
			data_end + // Length of the fields
			1; // End-of-record marker

	// Leader
	char leader[24];
	memset(leader, 0, 24);
	convertLeader(leader_desc, marc21_leader, leader);

	//	rec.GetLeader().Parse(leader, 24);
	char buff[6];
	sprintf(buff, "%05d", total);
	::memcpy(leader, buff, 5); // write record size
	sprintf(buff, "%05d", base_address);
	::memcpy(leader + MARCRecord<TLeader, TDir>::DIRECTORY_ENTRY_LEN, buff, 5); // write directory size
	os.write(leader, 24);

	// Directory
	std::copy(directory.begin(), directory.end(), std::ostream_iterator<std::string > (os, ""));
	os.put(MARCRecord<TLeader, TDir>::END_OF_FIELD);

	// Fields
	std::copy(fields.begin(), fields.end(), std::ostream_iterator<std::string > (os, ""));
	os.put(MARCRecord<TLeader, TDir>::END_OF_RECORD);

	return os;
}

#endif	/* MARCCONVERT_H */
