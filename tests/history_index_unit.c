#include "dunlin/history_index.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 dl_index *s=dl_index_open(":memory:");assert(s);
 assert(dl_index_put(s,"a","one",1,"我不喜欢苹果，Node foo_bar 2.1"));
 assert(dl_index_put(s,"b","private",1,"苹果私人信息"));
 char *r=dl_index_search(s,"a","苹果");assert(r&&strstr(r,"one")&&!strstr(r,"private")&&strstr(r,"不喜欢"));dl_index_free(r);
 r=dl_index_search(s,"a","NODE");assert(r&&strstr(r,"one"));dl_index_free(r);
 r=dl_index_search(s,"a","\" OR * \";DROP TABLE");assert(r);dl_index_free(r);
 assert(!dl_index_put(s,"a","bad",1,"\xF0\x80\x80\x80"));
 assert(dl_index_put(s,"a","one",1,"只保留香蕉"));
 r=dl_index_search(s,"a","苹果");assert(r&&!strcmp(r,"[]"));dl_index_free(r);
 assert(dl_index_delete(s,"a","one"));r=dl_index_search(s,"a","香蕉");assert(r&&!strcmp(r,"[]"));dl_index_free(r);
 dl_index_close(s);puts("PASS standalone C index: Chinese, original negation, scope, ASCII case, query escaping, malformed UTF-8, replacement and deletion");return 0;
}
