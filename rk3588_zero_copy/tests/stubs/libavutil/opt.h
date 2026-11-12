#pragma once
struct AVDictionary { int dummy; };
extern "C" {
int av_dict_set(AVDictionary** pm, const char* key, const char* value, int flags);
void av_dict_free(AVDictionary** m);
}
