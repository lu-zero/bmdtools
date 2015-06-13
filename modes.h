/*
 * Blackmagic Devices Decklink capture
 * Copyright (c) 2014 Luca Barbato.
 *
 * This file is part of bmdtools.
 *
 * bmdtools is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * bmdtools is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with bmdtools; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef BMDTOOLS_MODES_H
#define BMDTOOLS_MODES_H

#include "DeckLinkAPI.h"

#define CC_LINE 9

void print_input_modes(IDeckLink *deckLink);
void print_output_modes(IDeckLink *deckLink);

#endif /* BMDTOOLS_MODES_H */

