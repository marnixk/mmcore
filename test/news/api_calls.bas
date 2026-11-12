' --------------------------------------------------
'
'    Purpose: to interface with the API 
'
' --------------------------------------------------

dim base as string

base = "https://newsfeeds.co.nz"


'
'    Get a list of domains used for crawling
'
function DomainsGet() as DomainResponse
    local domainUrl as string
    
    domainUrl = base + "/api/collections/domains/records?page=1&perPage=10&sort=label"
    DomainsGet = web(http get domainUrl as DomainResponse)
end function




'
'    Get articles
'
function ArticlesGet() as ArticleResponse
    local articlesUrl as string
    articlesUrl = base + "/api/collections/crawl_pages/records?page=1&perPage=10&filter=ogType%20%3D%20%27article%27%20&sort=-created"
    ArticlesGet = web(http get articlesUrl as ArticleResponse)
end function