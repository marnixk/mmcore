' Few SUB/FUNCTIONs, so a cache miss does not scan a game-sized table.
' OPTION DEFAULT INTEGER matches Jetpack. Profiler stays off; TIMER is ms.
option default integer
dim a, b, i, n, t0, dt, k

n = 2000000
a = 1
b = 1
for i = 1 to 2000
  a = a + 1
next i

t0 = timer
for i = 1 to n
next i
dt = timer - t0
print "BENCH,empty_for,"; n; ","; dt

t0 = timer
for i = 1 to n
  a = a + 1
next i
dt = timer - t0
print "BENCH,int_inc,"; n; ","; dt

t0 = timer
for i = 1 to n
  b = (a < 1000000)
next i
dt = timer - t0
print "BENCH,int_lt,"; n; ","; dt

t0 = timer
for i = 1 to n
  k = keydown(0)
next i
dt = timer - t0
print "BENCH,keydown0,"; n; ","; dt

option tracecache off

t0 = timer
for i = 1 to n
next i
dt = timer - t0
print "BENCH,empty_for_off,"; n; ","; dt

t0 = timer
for i = 1 to n
  a = a + 1
next i
dt = timer - t0
print "BENCH,int_inc_off,"; n; ","; dt

t0 = timer
for i = 1 to n
  b = (a < 1000000)
next i
dt = timer - t0
print "BENCH,int_lt_off,"; n; ","; dt

print "DONE few"
end
