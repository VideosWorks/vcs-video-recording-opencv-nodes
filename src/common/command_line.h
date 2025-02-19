/*
 * 2018 Tarpeeksi Hyvae Soft /
 * VCS
 *
 */

#ifndef COMMAND_LINE_H
#define COMMAND_LINE_H

#include <string>

bool kcom_parse_command_line(const int argc, char *const argv[]);

const std::string& kcom_alias_file_name(void);

const std::string& kcom_filters_file_name(void);

const std::string& kcom_params_file_name(void);

#endif
