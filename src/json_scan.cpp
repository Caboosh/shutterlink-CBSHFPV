#include "json_scan.h"
#include <ctype.h>

static int jsonKeyIndex(const String &body, const char *key) {
    String pat = "\"";
    pat += key;
    pat += "\"";
    return body.indexOf(pat);
}

bool jsonHas(const String &body, const char *key) {
    return jsonKeyIndex(body, key) >= 0;
}

long jsonGetNum(const String &body, const char *key, long def) {
    int i = jsonKeyIndex(body, key);
    if (i < 0) return def;
    i = body.indexOf(':', i);
    if (i < 0) return def;
    i++;
    while (i < (int)body.length() && isspace((unsigned char)body[i])) i++;
    bool neg = false;
    if (body[i] == '-') { neg = true; i++; }
    long val = 0;
    while (i < (int)body.length() && isdigit((unsigned char)body[i])) {
        val = val * 10 + (body[i] - '0');
        i++;
    }
    return neg ? -val : val;
}

bool jsonGetBool(const String &body, const char *key, bool def) {
    int i = jsonKeyIndex(body, key);
    if (i < 0) return def;
    i = body.indexOf(':', i);
    if (i < 0) return def;
    int j = i + 1;
    while (j < (int)body.length() && isspace((unsigned char)body[j])) j++;
    if (body.startsWith("true", j))  return true;
    if (body.startsWith("false", j)) return false;
    return jsonGetNum(body, key, def ? 1 : 0) != 0;
}

String jsonGetStr(const String &body, const char *key) {
    int i = jsonKeyIndex(body, key);
    if (i < 0) return "";
    i = body.indexOf(':', i);
    if (i < 0) return "";
    i = body.indexOf('"', i);
    if (i < 0) return "";
    int j = body.indexOf('"', i + 1);
    if (j < 0) return "";
    return body.substring(i + 1, j);
}