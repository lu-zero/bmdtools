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

#include <stdio.h>
#include <stdlib.h>

#include "compat.h"
#include "DeckLinkAPI.h"
#include "Capture.h"

void print_display_mode(IDeckLinkDisplayMode *displayMode, int index)
{
    char modeName[64];
    int modeWidth, modeHeight;
    int pixelFormatIndex = 0;
    int ret;

    BMDTimeValue frameRateDuration;
    BMDTimeScale frameRateScale;
    // This seems to be deprecated in newer Decklink SDK 11
    // Since it is not needed in file it is commented out for now
    // BMDDisplayModeSupport displayModeSupport;
    BMDProbeString str;

    if (displayMode->GetName(&str) != S_OK)
        return;

    // Obtain the display mode's properties
    modeWidth  = displayMode->GetWidth();
    modeHeight = displayMode->GetHeight();
    displayMode->GetFrameRate(&frameRateDuration, &frameRateScale);

    printf("        %2d:   %-20s \t %d x %d \t %7g FPS\n",
           index, ToStr(str), modeWidth, modeHeight,
           (double)frameRateScale / (double)frameRateDuration);

    FreeStr(str);
}

void print_input_modes(IDeckLink *deckLink)
{
    IDeckLinkInput *deckLinkInput                     = NULL;
    IDeckLinkDisplayModeIterator *displayModeIterator = NULL;
    IDeckLinkDisplayMode *displayMode                 = NULL;
    HRESULT result;
    int index = 0;

    // Query the DeckLink for its configuration interface
    result = deckLink->QueryInterface(IID_IDeckLinkInput,
                                      (void **)&deckLinkInput);
    if (result != S_OK) {
        fprintf(
            stderr,
            "Could not obtain the IDeckLinkInput interface "
            "- result = %08x\n",
            result);
        goto bail;
    }

    // Obtain an IDeckLinkDisplayModeIterator to enumerate the display
    // modes supported on input
    result = deckLinkInput->GetDisplayModeIterator(&displayModeIterator);
    if (result != S_OK) {
        fprintf(
            stderr,
            "Could not obtain the video input display mode iterator "
            "- result = %08x\n",
            result);
        goto bail;
    }

    // List all supported output display modes
    printf("Supported video input display modes and pixel formats:\n");
    while (displayModeIterator->Next(&displayMode) == S_OK) {
        print_display_mode(displayMode, index++);
        displayMode->Release();
    }
bail:
    if (displayModeIterator != NULL) {
        displayModeIterator->Release();
    }
    if (deckLinkInput != NULL) {
        deckLinkInput->Release();
    }
}

void print_output_modes(IDeckLink *deckLink)
{
    IDeckLinkOutput *deckLinkOutput                   = NULL;
    IDeckLinkDisplayModeIterator *displayModeIterator = NULL;
    IDeckLinkDisplayMode *displayMode                 = NULL;
    HRESULT result;
    int index = 0;

    // Query the DeckLink for its configuration interface
    result = deckLink->QueryInterface(IID_IDeckLinkOutput,
                                      (void **)&deckLinkOutput);
    if (result != S_OK) {
        fprintf(
            stderr,
            "Could not obtain the IDeckLinkOutput interface - "
            "result = %08x\n",
            result);
        goto bail;
    }

    // Obtain an IDeckLinkDisplayModeIterator to enumerate the display
    // modes supported on output
    result = deckLinkOutput->GetDisplayModeIterator(&displayModeIterator);
    if (result != S_OK) {
        fprintf(
            stderr,
            "Could not obtain the video output display mode iterator - "
            "result = %08x\n",
            result);
        goto bail;
    }

    // List all supported output display modes
    printf("Supported video output display modes and pixel formats:\n");
    while (displayModeIterator->Next(&displayMode) == S_OK) {
        print_display_mode(displayMode, index++);
        displayMode->Release();
    }
bail:
    if (displayModeIterator != NULL)
        displayModeIterator->Release();
    if (deckLinkOutput != NULL)
        deckLinkOutput->Release();
}
