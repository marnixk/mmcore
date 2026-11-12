' --------------------------------------------------
'
'    Domain description
'
' --------------------------------------------------
type DomainRecord
    id as string
    label as string
end type

' --------------------------------------------------
'
'    Describes an article
'
' --------------------------------------------------
type ArticleRecord
    domainLabel as string
    author as string
    description as string
    keywords as string
    summary as string
    sntiment as string
    updated as string
end type

' --------------------------------------------------
'
'    Describes a record
'
' --------------------------------------------------
type DossierRecord
    title as string
    summary as string
    heat as integer
    body as string
    key_points(20) as string
end type


' --------------------------------------------------
'
'    Response for getting a list of domains
'   
' --------------------------------------------------
type DomainResponse
    items(10) as DomainRecord
    totalPages as integer
    totalItems as integer
    page as integer
    perPage as integer
end type

' --------------------------------------------------
'
'    Article response
'   
' --------------------------------------------------
type ArticleResponse
    items(50) as ArticleRecord
    totalPages as integer
    totalItems as integer
    page as integer
    perPage as integer
end type

' --------------------------------------------------
'
'    Dossier response
'   
' --------------------------------------------------
type DossierResponse
    items(50) as DossierRecord
    totalPages as integer
    totalItems as integer
    page as integer
    perPage as integer
end type
