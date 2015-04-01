/* 
 * File:   main.cpp
 * Author: fragoulis
 *
 * Created on 21 Μάϊος 2013, 11:20 πμ
 */

#include <cstdlib>
#include <iostream>
#include <exception>
#include <algorithm>
#include "MarConvertor.h"

using namespace std;

#include <boost/xpressive/xpressive.hpp>
using namespace boost::xpressive;

/*
 * 
 */
int main(int argc, char** argv) {

	const char *src = "input.mrc";
	const char *dst = "output.mrc";
	const char *rules = "rules.json";
	if (argc == 4) {
		src = argv[1];
		dst = argv[2];
		rules = argv[3];
	}

	try {
		MarConvertor* c = new MarConvertor();
		c->convertMarc21ToUnimarc(src, dst, rules);
		delete c;

	} catch (const std::exception& e) {
		std::cerr << e.what() << std::endl;
	} catch (...) {
		std::cerr << "Unknown exception occurred" << std::endl;
	}
	char k; cin >> k;
	return 0;
}

