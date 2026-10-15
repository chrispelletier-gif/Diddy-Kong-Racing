/*====================================================================
 * bnkf.c
 *
 * Copyright 1993, Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This is UNPUBLISHED PROPRIETARY SOURCE CODE of Silicon Graphics,
 * Inc.; the contents of this file may not be disclosed to third
 * parties, copied or duplicated in any form, in whole or in part,
 * without the prior written permission of Silicon Graphics, Inc.
 *
 * RESTRICTED RIGHTS LEGEND:
 * Use, duplication or disclosure by the Government is subject to
 * restrictions as set forth in subdivision (c)(1)(ii) of the Rights
 * in Technical Data and Computer Software clause at DFARS
 * 252.227-7013, and/or in similar or successor clauses in the FAR,
 * DOD or NASA FAR Supplement. Unpublished - rights reserved under the
 * Copyright Laws of the United States.
 *====================================================================*/

#include <libaudio.h>
#include <os_internal.h>
#include <ultraerror.h>

/*
 * ### when the file format settles down a little, I'll remove these
 * ### for efficiency.
 */
static  void _bnkfPatchBank(ALBank *bank, s32 offset, s32 table);
static  void _bnkfPatchInst(ALInstrument *i, s32 offset, s32 table);
static  void _bnkfPatchSound(ALSound *s, s32 offset, s32 table);
static  void _bnkfPatchWaveTable(ALWaveTable *w, s32 offset, s32 table);

void alSeqFileNew(ALSeqFile *file, u8 *base)
{
    s32 offset = (s32) base;
    s32 i;
    
    /*
     * patch the file so that offsets are pointers
     */
    for (i = 0; i < file->seqCount; i++) {
        file->seqArray[i].offset = (u8 *)((u8 *)file->seqArray[i].offset + offset);
    }
}

void alBnkfNew(ALBankFile *file, u8 *table)
{
    s32 offset = (s32) file;
    s32 woffset = (s32) table;
    
    s32 i;
    
    /*
     * check the file format revision in debug libraries
     */
#ifdef TARGET_PORT
    port_swap_bank_file(file);
#endif
    ALFailIf(file->revision != AL_BANK_VERSION, ERR_ALBNKFNEW);
    
    /*
     * patch the file so that offsets are pointers
     */
    for (i = 0; i < file->bankCount; i++) {
        file->bankArray[i] = (ALBank *)((u8 *)file->bankArray[i] + offset);
        if(file->bankArray[i])
            _bnkfPatchBank(file->bankArray[i], offset, woffset);
    }
}

void _bnkfPatchBank(ALBank *bank, s32 offset, s32 table) 
{
    s32 i;
    
    if (bank->flags)
        return;

    bank->flags = 1;
#ifdef TARGET_PORT
    port_swap_bank(bank);
#endif

    if (bank->percussion) {
        bank->percussion = (ALInstrument *)((u8 *)bank->percussion + offset);
        _bnkfPatchInst(bank->percussion, offset, table);
    }
    
    for (i = 0; i < bank->instCount; i++) {
        bank->instArray[i] = (ALInstrument *)((u8 *)bank->instArray[i] +
                                              offset);
        if(bank->instArray[i])
            _bnkfPatchInst(bank->instArray[i], offset, table);
    }
}

void _bnkfPatchInst(ALInstrument *inst, s32 offset, s32 table)
{
    s32 i;

    if (inst->flags)
        return;

    inst->flags = 1;
#ifdef TARGET_PORT
    port_swap_instrument(inst);
#endif
    
    for (i = 0; i < inst->soundCount; i++) {
        inst->soundArray[i] = (ALSound *)((u8 *)inst->soundArray[i] +
                                          offset);
        _bnkfPatchSound(inst->soundArray[i], offset, table);

    }
}

void _bnkfPatchSound(ALSound *s, s32 offset, s32 table)
{
    if (s->flags)
        return;

    s->flags = 1;
#ifdef TARGET_PORT
    port_swap_sound(s);
#endif
    
    s->envelope  = (ALEnvelope *)((u8 *)s->envelope + offset);
#ifdef TARGET_PORT
    port_swap_envelope(s->envelope);
#endif
    s->keyMap    = (ALKeyMap *)((u8 *)s->keyMap + offset);

    s->wavetable = (ALWaveTable *)((u8 *)s->wavetable + offset);
    _bnkfPatchWaveTable(s->wavetable, offset, table);
}

void _bnkfPatchWaveTable(ALWaveTable *w, s32 offset, s32 table)
{
    if (w->flags)
        return;

    w->flags = 1;
#ifdef TARGET_PORT
    port_swap_wavetable(w);
#endif
    
    w->base += table;

    /* sct 2/14/96 - patch wavetable loop info based on type. */
    if (w->type == AL_ADPCM_WAVE)
    {
	w->waveInfo.adpcmWave.book  = (ALADPCMBook *)((u8 *)w->waveInfo.adpcmWave.book + offset);
	if (w->waveInfo.adpcmWave.loop)
	    w->waveInfo.adpcmWave.loop = (ALADPCMloop *)((u8 *)w->waveInfo.adpcmWave.loop + offset);
#ifdef TARGET_PORT
	port_swap_adpcm_book(w->waveInfo.adpcmWave.book);
	if (w->waveInfo.adpcmWave.loop)
	    port_swap_adpcm_loop(w->waveInfo.adpcmWave.loop);
#endif
    }
    else if (w->type == AL_RAW16_WAVE)
    {
	if (w->waveInfo.rawWave.loop)
	    w->waveInfo.rawWave.loop = (ALRawLoop *)((u8 *)w->waveInfo.rawWave.loop + offset);
#ifdef TARGET_PORT
	if (w->waveInfo.rawWave.loop)
	    port_swap_raw_loop(w->waveInfo.rawWave.loop);
#endif
    }	
}
