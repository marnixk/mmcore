' PAUSE-heavy loop (Jetpack's gm.play spins PAUSE IDLE_MS). The storage
' hotplug walk used to run on every spin; sample this process to compare.
option default integer
dim i, n
n = 4000
for i = 1 to n
  pause 4
next i
print "DONE pause"
end
