'
'    Read newsfeeds.co.nz
'

#include "types.bas"
#include "api_calls.bas"

dim domains as DomainResponse
dim articles as ArticleResponse

domains = DomainsGet()
articles = ArticlesGet()

dim idx as integer

for idx = 0 to domains.totalItems - 1
    print domains.items(idx).label
next idx


print "Articles: "; articles.totalItems