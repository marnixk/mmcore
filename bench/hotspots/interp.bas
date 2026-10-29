' Jetpack-sized SUB table (~100 unused FUNCTIONs) plus the constructs the
' play loop actually uses. OPTION DEFAULT INTEGER. Profiler off.
option default integer
dim a, b, c, d, i, n, m, t0, dt, k, x, y, g
dim float f
dim string s$
dim ar(64)
dim grid(32, 16)

a = 3
b = 5
c = 0
d = 0
x = 4
y = 6
f = 1
ar(x) = 1
grid(x, y) = 1
pl.x = 1

n = 1500000
m = 200000

for i = 1 to 1000
  a = a + 1
next i

t0 = timer
for i = 1 to n
next i
dt = timer - t0
print "BENCH,empty_for,"; n; ","; dt

t0 = timer
for i = 1 to m
next i
dt = timer - t0
print "BENCH,empty_for,"; m; ","; dt

t0 = timer
for i = 1 to n
  a% = a% + 1
next i
dt = timer - t0
print "BENCH,suffix_inc,"; n; ","; dt

t0 = timer
for i = 1 to n
  a = a + 1
next i
dt = timer - t0
print "BENCH,int_inc,"; n; ","; dt

t0 = timer
for i = 1 to n
  f = f + 1
next i
dt = timer - t0
print "BENCH,float_inc,"; n; ","; dt

t0 = timer
for i = 1 to n
  pl.x = pl.x + 1
next i
dt = timer - t0
print "BENCH,dotted_inc,"; n; ","; dt

t0 = timer
for i = 1 to n
  b = (a < 1000000)
next i
dt = timer - t0
print "BENCH,int_lt,"; n; ","; dt

t0 = timer
for i = 1 to n
  if a < 1000000 then b = b + 1
next i
dt = timer - t0
print "BENCH,if_lt,"; n; ","; dt

t0 = timer
for i = 1 to n
  c = b \ 7
next i
dt = timer - t0
print "BENCH,idiv,"; n; ","; dt

t0 = timer
for i = 1 to n
  c = b mod 7
next i
dt = timer - t0
print "BENCH,mod,"; n; ","; dt

t0 = timer
for i = 1 to n
  ar(x) = ar(x) + 1
next i
dt = timer - t0
print "BENCH,arr1,"; n; ","; dt

t0 = timer
for i = 1 to n
  grid(x, y) = grid(x, y) + 1
next i
dt = timer - t0
print "BENCH,arr2,"; n; ","; dt

t0 = timer
for i = 1 to m
  if a = 1 then
    b = 1
  elseif a = 2 then
    b = 2
  elseif a = 3 then
    b = 3
  else
    b = 4
  end if
next i
dt = timer - t0
print "BENCH,if_chain,"; m; ","; dt

t0 = timer
for i = 1 to m
  select case a
    case 1
      b = 1
    case 2
      b = 2
    case 3
      b = 3
    case else
      b = 4
  end select
next i
dt = timer - t0
print "BENCH,select_case,"; m; ","; dt

t0 = timer
for i = 1 to m
  gosub glab
next i
dt = timer - t0
print "BENCH,gosub,"; m; ","; dt

t0 = timer
for i = 1 to m
  nosub
next i
dt = timer - t0
print "BENCH,sub_empty,"; m; ","; dt

t0 = timer
for i = 1 to m
  bump
next i
dt = timer - t0
print "BENCH,sub_body,"; m; ","; dt

t0 = timer
for i = 1 to m
  useslocal
next i
dt = timer - t0
print "BENCH,sub_local,"; m; ","; dt

t0 = timer
for i = 1 to m
  b = add1(b)
next i
dt = timer - t0
print "BENCH,fn_add1,"; m; ","; dt

t0 = timer
for i = 1 to m
  k = keydown(0)
next i
dt = timer - t0
print "BENCH,keydown0,"; m; ","; dt

t0 = timer
for i = 1 to m
  k = isdown(131)
next i
dt = timer - t0
print "BENCH,isdown,"; m; ","; dt

t0 = timer
for i = 1 to m
  s$ = "jet" + "pack"
next i
dt = timer - t0
print "BENCH,str_cat,"; m; ","; dt

t0 = timer
for i = 1 to m
  s$ = mid$("jetpack!", 2, 4)
next i
dt = timer - t0
print "BENCH,mid$,"; m; ","; dt

t0 = timer
for i = 1 to m
  k = mm.hres
next i
dt = timer - t0
print "BENCH,mm_hres,"; m; ","; dt

t0 = timer
for i = 1 to m
  k = mm.info(mode)
next i
dt = timer - t0
print "BENCH,mm_info_mode,"; m; ","; dt

option tracecache off
t0 = timer
for i = 1 to m
next i
dt = timer - t0
print "BENCH,empty_for,"; m; ","; dt

t0 = timer
for i = 1 to m
  a = a + 1
next i
dt = timer - t0
print "BENCH,int_inc_off,"; m; ","; dt

t0 = timer
for i = 1 to m
  pl.x = pl.x + 1
next i
dt = timer - t0
print "BENCH,dotted_inc_off,"; m; ","; dt

t0 = timer
for i = 1 to m
  grid(x, y) = grid(x, y) + 1
next i
dt = timer - t0
print "BENCH,arr2_off,"; m; ","; dt

t0 = timer
for i = 1 to m
  b = (a < 1000000)
next i
dt = timer - t0
print "BENCH,int_lt_off,"; m; ","; dt

t0 = timer
for i = 1 to m
  k = keydown(0)
next i
dt = timer - t0
print "BENCH,keydown0_off,"; m; ","; dt

print "DONE interp a="; a; " b="; b
end

glab:
  g = g + 1
  return

sub nosub()
end sub

sub bump()
  g = g + 1
end sub

sub useslocal()
  local t
  t = t + 1
end sub

function add1(nn)
  add1 = nn + 1
end function

function isdown(code)
  local ii
  isdown = 0
  for ii = 1 to keydown(0)
    if keydown(ii) = code then isdown = 1
  next ii
end function

function zfn00()
  zfn00 = 0
end function

function zfn01()
  zfn01 = 0
end function

function zfn02()
  zfn02 = 0
end function

function zfn03()
  zfn03 = 0
end function

function zfn04()
  zfn04 = 0
end function

function zfn05()
  zfn05 = 0
end function

function zfn06()
  zfn06 = 0
end function

function zfn07()
  zfn07 = 0
end function

function zfn08()
  zfn08 = 0
end function

function zfn09()
  zfn09 = 0
end function

function zfn10()
  zfn10 = 0
end function

function zfn11()
  zfn11 = 0
end function

function zfn12()
  zfn12 = 0
end function

function zfn13()
  zfn13 = 0
end function

function zfn14()
  zfn14 = 0
end function

function zfn15()
  zfn15 = 0
end function

function zfn16()
  zfn16 = 0
end function

function zfn17()
  zfn17 = 0
end function

function zfn18()
  zfn18 = 0
end function

function zfn19()
  zfn19 = 0
end function

function zfn20()
  zfn20 = 0
end function

function zfn21()
  zfn21 = 0
end function

function zfn22()
  zfn22 = 0
end function

function zfn23()
  zfn23 = 0
end function

function zfn24()
  zfn24 = 0
end function

function zfn25()
  zfn25 = 0
end function

function zfn26()
  zfn26 = 0
end function

function zfn27()
  zfn27 = 0
end function

function zfn28()
  zfn28 = 0
end function

function zfn29()
  zfn29 = 0
end function

function zfn30()
  zfn30 = 0
end function

function zfn31()
  zfn31 = 0
end function

function zfn32()
  zfn32 = 0
end function

function zfn33()
  zfn33 = 0
end function

function zfn34()
  zfn34 = 0
end function

function zfn35()
  zfn35 = 0
end function

function zfn36()
  zfn36 = 0
end function

function zfn37()
  zfn37 = 0
end function

function zfn38()
  zfn38 = 0
end function

function zfn39()
  zfn39 = 0
end function

function zfn40()
  zfn40 = 0
end function

function zfn41()
  zfn41 = 0
end function

function zfn42()
  zfn42 = 0
end function

function zfn43()
  zfn43 = 0
end function

function zfn44()
  zfn44 = 0
end function

function zfn45()
  zfn45 = 0
end function

function zfn46()
  zfn46 = 0
end function

function zfn47()
  zfn47 = 0
end function

function zfn48()
  zfn48 = 0
end function

function zfn49()
  zfn49 = 0
end function

function zfn50()
  zfn50 = 0
end function

function zfn51()
  zfn51 = 0
end function

function zfn52()
  zfn52 = 0
end function

function zfn53()
  zfn53 = 0
end function

function zfn54()
  zfn54 = 0
end function

function zfn55()
  zfn55 = 0
end function

function zfn56()
  zfn56 = 0
end function

function zfn57()
  zfn57 = 0
end function

function zfn58()
  zfn58 = 0
end function

function zfn59()
  zfn59 = 0
end function

function zfn60()
  zfn60 = 0
end function

function zfn61()
  zfn61 = 0
end function

function zfn62()
  zfn62 = 0
end function

function zfn63()
  zfn63 = 0
end function

function zfn64()
  zfn64 = 0
end function

function zfn65()
  zfn65 = 0
end function

function zfn66()
  zfn66 = 0
end function

function zfn67()
  zfn67 = 0
end function

function zfn68()
  zfn68 = 0
end function

function zfn69()
  zfn69 = 0
end function

function zfn70()
  zfn70 = 0
end function

function zfn71()
  zfn71 = 0
end function

function zfn72()
  zfn72 = 0
end function

function zfn73()
  zfn73 = 0
end function

function zfn74()
  zfn74 = 0
end function

function zfn75()
  zfn75 = 0
end function

function zfn76()
  zfn76 = 0
end function

function zfn77()
  zfn77 = 0
end function

function zfn78()
  zfn78 = 0
end function

function zfn79()
  zfn79 = 0
end function

function zfn80()
  zfn80 = 0
end function

function zfn81()
  zfn81 = 0
end function

function zfn82()
  zfn82 = 0
end function

function zfn83()
  zfn83 = 0
end function

function zfn84()
  zfn84 = 0
end function

function zfn85()
  zfn85 = 0
end function

function zfn86()
  zfn86 = 0
end function

function zfn87()
  zfn87 = 0
end function

function zfn88()
  zfn88 = 0
end function

function zfn89()
  zfn89 = 0
end function

function zfn90()
  zfn90 = 0
end function

function zfn91()
  zfn91 = 0
end function

function zfn92()
  zfn92 = 0
end function

function zfn93()
  zfn93 = 0
end function

function zfn94()
  zfn94 = 0
end function

function zfn95()
  zfn95 = 0
end function

function zfn96()
  zfn96 = 0
end function

function zfn97()
  zfn97 = 0
end function

function zfn98()
  zfn98 = 0
end function

function zfn99()
  zfn99 = 0
end function
