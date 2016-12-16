/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker

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

#ifndef FDC_H
#define FDC_H

extern void fdc_reset(void);
extern void fdc_callback(void);
extern uint8_t fdc_dma_read(uint32_t addr);
extern void fdc_dma_write(uint32_t addr, uint8_t val);
extern void fdc_adf_load(const char *fn, int drive);
extern void fdc_adf_save(const char *fn, int drive);
extern uint8_t fdc_read(uint32_t addr);
extern void fdc_write(uint32_t addr, uint32_t val);

extern int fdccallback;
extern int motoron;

#endif /* FDC_H */
