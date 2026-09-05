

'
' Fade the sound out
'
sub fades.soundFadeOut()
  local integer vPercent

  for vPercent = 100 to 0 step -1
    play volume vPercent, vPercent
    pause 2
  next vPercent

end sub



'
' Fade the sound in
'
sub fades.soundFadeIn()
  local integer vPercent

  for vPercent = 0 to 100
    play volume vPercent, vPercent
    pause 2
  next vPercent

end sub




sub fades.fadeInMiddle()

  '
  ' make sure we're writing on the transparency layer
  '
  page write 1

  '
  ' Write a band up the screen
  '
  local integer row, tp 


  for row = 0 to 120 + 15 
    for tp = 0 to 15
      line 0, 120 - row - tp, 320, 120 - row - tp, 1, rgb(0, 0, 0, tp)
      line 0, 120 + row + tp, 320, 120 + row + tp, 1, rgb(0, 0, 0, tp)
    next tp

    pause 3
  next row
  
end sub



sub fades.fadeIn()

  '
  ' make sure we're writing on the transparency layer
  '
  page write 1

  '
  ' Write a band up the screen
  '
  local integer row, tp 


  for row = 240 + 15 to 0 step -1
    for tp = 0 to 15
      line 0, row - tp, 320, row - tp, 1, rgb(0, 0, 0, tp)
    next tp

    pause 3
  next row
  
end sub



sub fades.fadeOut()

  '
  ' make sure we're writing on the transparency layer
  '
  page write 1

  '
  ' Write a band up the screen
  '
  local integer row, tp 

  for row = 240 + 15 to 0 step -1
    for tp = 0 to 15
      line 0, row - tp, 320, row - tp, 1, rgb(0, 0, 0, 15 - tp)
    next tp

    pause 3
  next row
  
end sub

