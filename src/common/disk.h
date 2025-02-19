#ifndef DISK_H_
#define DISK_H_

#include <vector>

class FilterGraphNode;
class QString;

struct filter_graph_option_s;
struct video_mode_params_s;
struct mode_alias_s;

bool kdisk_save_video_mode_params(const std::vector<video_mode_params_s> &modeParams, const QString &targetFilename);
bool kdisk_save_filter_graph(std::vector<FilterGraphNode*> &nodes, std::vector<filter_graph_option_s> &options, const QString &targetFilename);
bool kdisk_save_aliases(const std::vector<mode_alias_s> &aliases, const QString &targetFilename);

bool kdisk_load_video_mode_params(const std::string &sourceFilename);
bool kdisk_load_filter_graph(const std::string &sourceFilename);
bool kdisk_load_aliases(const std::string &sourceFilename);

#endif
