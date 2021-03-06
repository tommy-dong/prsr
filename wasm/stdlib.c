#include <string.h>  // just for types

int isalpha(int c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int isdigit(int c) {
  return (c >= '0' && c <= '9');
}

int isalnum(int c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

int isspace(int c) {
  return c == ' ' || (c >= '\t' && c <= '\r');  // \t, \n, \v, \f, \r
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const unsigned char *p1 = s1, *p2 = s2;
  while (n--) {
    if (*p1 != *p2) {
      return *p1 - *p2;
    }
    p1++;
    p2++;
  }
  return 0;
}

void *memchr(const void *s, int c, size_t n) {
  unsigned char *p = (unsigned char *) s;
  while (n--) {
    if (*p == (unsigned char) c) {
      return p;
    }
    ++p;
  }
  return 0;
}
