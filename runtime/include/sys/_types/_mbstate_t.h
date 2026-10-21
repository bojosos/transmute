#ifndef ALCHEMY_MBSTATE_T_H
#define ALCHEMY_MBSTATE_T_H
typedef union __mbstate_t { char opaque[128]; long long alignment; } mbstate_t;
#endif
