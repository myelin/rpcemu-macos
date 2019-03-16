/*
 RPCEmu - An Acorn system emulator
 
 Copyright (C) 2017 Peter Howkins
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef __EVENTS_MACOSX_H__
#define __EVENTS_MACOSX_H__

#ifdef __cplusplus
extern "C" {
#endif
    
typedef enum {
    nativeEventTypeModifiersChanged = 1
} NativeEventType;
    
typedef struct
{
    bool processed;
    int eventType;
    uint modifierMask;
} NativeEvent;
    
extern NativeEvent* handle_native_event(void *message);
    
#ifdef __cplusplus
}
#endif

#endif // __EVENTS_MACOSX_H__
