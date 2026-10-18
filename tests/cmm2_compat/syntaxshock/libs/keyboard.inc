const NumberOfKeys = 9

' keyboard state
dim integer keyboard(NumberOfKeys)


' define the indices of the keyboard array
const K_Enter = 0, K_Right = 1, K_Left = 2, K_Up = 3, K_Down = 4, K_Z = 5, K_V = 6, K_Escape = 7, K_Middle = 9


'
' Simply reads all elements available in the keyboard buffer
'
sub keyb.DrainBuffer
  
  do: loop until inkey$ = ""

end sub

'
' Reset the state of the keyboard
'
sub keyb.Initialise
  local integer idx
  for idx = 0 to NumberOfKeys 
    keyboard(idx) = False
  next idx  
end sub

'
' Goes over the information in keydown() and makes sure to update
' the keyboard state information array. First thing it does is
' reset the keyboard state.
'
sub keyb.GatherKeyInfo
  
  local integer idx
  for idx = 0 to NumberOfKeys 
    keyboard(idx) = False
  next idx  

  ' KEYDOWN with a scan code tests that PC/AT set-1 key.
  ' See docs/help/keyboard-scan-codes.txt.
  if keydown(47) then keyboard(K_V) = True       ' V
  if keydown(44) then keyboard(K_Z) = True       ' Z
  if keydown(75) then keyboard(K_Left) = True    ' Left arrow
  if keydown(77) then keyboard(K_Right) = True   ' Right arrow
  if keydown(72) then keyboard(K_Up) = True      ' Up arrow
  if keydown(80) then keyboard(K_Down) = True    ' Down arrow
  if keydown(6) then keyboard(K_Middle) = True   ' top-row 5
  if keydown(28) then keyboard(K_Enter) = True   ' Enter
  if keydown(1) then keyboard(K_Escape) = True   ' Esc

end sub


on key keyb.DrainBuffer
