' Minimal graphics loop for MMCORE_FRAME_PROFILE / OPTION FRAMEPROFILE.
' After RUN, read C:/.mmcore-frame-profile.csv (or A: if C: is missing).
option explicit
option frameprofile on
mode 2,16
page write 1
for i = 1 to 40
  cls rgb(0,0,32)
  box 10+i, 40, 30, 30, 1, rgb(200,80,40), 0
  page copy 1 to 0
  pause 5
next i
end
