const NumberOfKeys = 9

' keyboard state
dim integer keyboard(NumberOfKeys)


' define the scan codes for the keys we're interested in
const SC_Enter = 10, SC_Right = 131, SC_Left = 130, SC_Up = 128, SC_Down = 129, SC_V = 118, SC_Z = 122, SC_Escape = 27
const SC_Spacebar = 32, SC_Middle = 53

' define the indices of the keyboard array
const K_Enter = 0, K_Right = 1, K_Left = 2, K_Up = 3, K_Down = 4, K_Z = 5, K_V = 6, K_Escape = 7, K_Spacebar = 8, K_Middle = 9


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

  ' iterate over the keystates that have a value 
  for idx = 1 to keydown(0)

    ' debug
    ' text 0, 20 + idx * 10, str$(keydown(idx))

    ' if scan code found setup the state
    select case keydown(idx)
      case SC_V: keyboard(K_V) = True
      case SC_Z: keyboard(K_Z) = True
      case SC_Left: keyboard(K_Left) = True
      case SC_Right: keyboard(K_Right) = True
      case SC_Up: keyboard(K_Up) = True
      case SC_Down: keyboard(K_Down) = True
      case SC_Middle: keyboard(K_Middle) = True
      case SC_Enter: keyboard(K_Enter) = True
      case SC_Escape: keyboard(K_Escape) = True
    end select    
  next idx

end sub


on key keyb.DrainBuffer
