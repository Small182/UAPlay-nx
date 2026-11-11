#include "../source/catalog.c"
#include <assert.h>
#include <stdio.h>
int main(void){
    Source items[40]={0};
    int n=0;
    catalog_http_init();
    search_animeon("Мисливець х Мисливець",items,&n,40);
    catalog_http_cleanup();
    for(int i=0;i<n;i++)printf("%s\n",items[i].title);
    assert(n>=1 && n<=3);
    return 0;
}
