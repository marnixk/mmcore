' MODE 20 graphics isolates. TIMER is ms. Empty FOR is subtracted later.
option default integer
mode 20, 16
page write 1
cls rgb(20, 40, 80)
box 0, 0, 220, 120, 1, rgb(200, 40, 40), rgb(200, 40, 40)
page write 0
cls rgb(0, 0, 0)
sprite read 1, 0, 0, 32, 32, 1

dim i, n, t0, dt

print "BENCH,mode,"; mm.hres; ","; mm.vres

n = 400
t0 = timer
for i = 1 to n
next i
dt = timer - t0
print "BENCH,empty_for,"; n; ","; dt

t0 = timer
for i = 1 to n
  page copy 1 to 0
next i
dt = timer - t0
print "BENCH,page_copy,"; n; ","; dt

n = 100000
t0 = timer
for i = 1 to n
next i
dt = timer - t0
print "BENCH,empty_for,"; n; ","; dt

t0 = timer
for i = 1 to n
  blit 0, 0, 40, 40, 32, 32, 1
next i
dt = timer - t0
print "BENCH,blit_32,"; n; ","; dt

n = 20000
t0 = timer
for i = 1 to n
next i
dt = timer - t0
print "BENCH,empty_for,"; n; ","; dt

t0 = timer
for i = 1 to n
  blit 0, 0, 8, 8, 160, 96, 1
next i
dt = timer - t0
print "BENCH,blit_160x96,"; n; ","; dt

n = 20000
sprite show 1, 100, 100, 1
t0 = timer
for i = 1 to n
next i
dt = timer - t0
print "BENCH,empty_for,"; n; ","; dt

t0 = timer
for i = 1 to n
  sprite next 1, 80 + (i mod 16), 140
  sprite move
next i
dt = timer - t0
print "BENCH,sprite_next_move,"; n; ","; dt

print "DONE gfx"
end
