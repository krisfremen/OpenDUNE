/** @file src/audio/mt32mpu.c MPU routines. */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "multichar.h"
#include "types.h"
#include "../os/common.h"
#include "../os/math.h"
#include "../os/endian.h"
#include "../os/error.h"
#include "../os/sleep.h"
#include "../os/thread.h"

#include "mt32mpu.h"

#include "midi.h"

#if defined(_WIN32)
static Semaphore s_mpu_sem = NULL;
static Thread s_mpu_thread = NULL;
static uint32 s_mpu_usec = 0;
#endif /* _WIN32 */

typedef struct Controls {
	uint8 volume;
	uint8 modulation;
	uint8 panpot;
	uint8 expression;
	uint8 sustain;
	uint8 patch_bank_sel;
	uint8 chan_lock;
	uint8 chan_protect;
	uint8 voice_protect;
} Controls;

typedef struct MSData {
	uint8 *EVNT;                                            /*!< Pointer to EVNT position in sound file. */
	uint8 *sound;                                           /*!< Pointer to current position in sound file. */
	uint16 playing;                                         /*!< ?? 0, 1 or 2. 1 if a sound is playing. */
	bool   delayedClear;                                    /*!< ?? */
	int16  delay;                                           /*!< Delay before reading next command. */
	uint16 noteOnCount;                                     /*!< Number of notes currently on. */
	uint16 variable_0022;                                   /*!< ?? */
	uint16 variable_0024;                                   /*!< ?? */
	uint16 variable_0026;                                   /*!< ?? */
	uint32 variable_0028;                                   /*!< ?? */
	uint32 variable_002C;                                   /*!< ?? */
	uint16 variable_0030;                                   /*!< ?? */
	uint16 variable_0032;                                   /*!< ?? */
	uint16 variable_0034;                                   /*!< ?? */
	uint32 variable_0036;                                   /*!< ?? */
	uint32 variable_003A;                                   /*!< ?? */
	uint16 variable_003E;                                   /*!< ?? */
	uint16 variable_0040;                                   /*!< ?? */
	uint16 variable_0042;                                   /*!< ?? */
	uint32 variable_0044;                                   /*!< ?? */
	uint32 variable_0048;                                   /*!< ?? */
	uint32 variable_004C;                                   /*!< ?? */
	uint8 *variable_0050[4];                                /*!< ?? */
	uint16 variable_0060[4];                                /*!< ?? */
	uint8  chanMaps[16];                                    /*!< ?? Channel mapping. */
	Controls controls[16];                                  /*!< ?? */
	uint8  noteOnChans[32];                                 /*!< ?? */
	uint8  noteOnNotes[32];                                 /*!< ?? */
	int32  noteOnDuration[32];                              /*!< ?? */
} MSData;

static MSData *s_mpu_msdata[8];
static uint16 s_mpu_msdataSize;
static uint16 s_mpu_msdataCurrent;

static Controls s_mpu_controls[16];
static uint8 s_mpu_programs[16];
static uint16 s_mpu_pitchWheel[16];
static uint8 s_mpu_noteOnCount[16];
static uint8 s_mpu_lockStatus[16];
static bool s_mpu_initialized;

static bool s_mpuIgnore = false;

static void MPU_Send(uint8 status, uint8 data1, uint8 data2)
{
	s_mpuIgnore = true;
	midi_send(status | (data1 << 8) | (data2 << 16));
	s_mpuIgnore = false;
}

static void MPU_ApplyVolume(MSData *data)
{
	uint8 i;

	for (i = 0; i < 16; i++) {
		uint8 volume;

		volume = data->controls[i].volume;
		if (volume == 0xFF) continue;

		volume = min((volume * data->variable_0024) / 100, 127);

		s_mpu_controls[i].volume = volume;

		if ((s_mpu_lockStatus[i] & 0x80) != 0) continue;

		MPU_Send(0xB0 | data->chanMaps[i], 7, volume);
	}
}

static uint16 MPU_NoteOn(MSData *data)
{
	uint8 chan;
	uint8 note;
	uint8 velocity;
	uint8 *sound;
	uint16 len = 0;
	uint32 duration = 0;
	uint8 i;

	sound = data->sound;

	chan = *sound++ & 0xF;
	note = *sound++;
	velocity = *sound++;

	while (true) {
		uint8 v = *sound++;
		duration |= v & 0x7F;
		if ((v & 0x80) == 0) break;
		duration <<= 7;
	}

	len = (uint16)(sound - data->sound);

	if ((s_mpu_lockStatus[chan] & 0x80) != 0) return len;

	for (i = 0; i < 32; i++) {
		if (data->noteOnChans[i] == 0xFF) {
			data->noteOnCount++;
			break;
		}
	}
	if (i == 32) i = 0;

	data->noteOnChans[i] = chan;
	data->noteOnNotes[i] = note;
	data->noteOnDuration[i] = duration - 1;

	chan = data->chanMaps[chan];
	s_mpu_noteOnCount[chan]++;

	/* Note On */
	MPU_Send(0x90 | chan, note, velocity);

	return len;
}

static void MPU_FlushChannel(uint8 channel)
{
	uint16 count;
	uint16 index = 0;

	count = s_mpu_msdataSize;

	while (count-- != 0) {
		MSData *data;
		uint8 i;

		while (s_mpu_msdata[index] == NULL) index++;

		data = s_mpu_msdata[index];

		if (data->noteOnCount == 0) continue;

		for (i = 0; i < 32; i++) {
			uint8 chan;
			uint8 note;

			chan = data->noteOnChans[i];
			if (chan != channel) continue;
			data->noteOnChans[i] = 0xFF;

			note = data->noteOnNotes[i];
			chan = data->chanMaps[chan];
			s_mpu_noteOnCount[chan]--;

			/* Note Off */
			MPU_Send(0x80 | chan, note, 0);

			data->noteOnCount--;
		}
	}
}

static uint8 MPU_281A(void)
{
	uint8 i;
	uint8 chan = 0xFF;
	uint8 flag = 0xC0;
	uint8 min = 0xFF;

	while (true) {
		for (i = 0; i < 16; i++) {
			if ((s_mpu_lockStatus[15 - i] & flag) == 0 && s_mpu_noteOnCount[15 - i] < min) {
				min = s_mpu_noteOnCount[15 - i];
				chan = 15 - i;
			}
		}
		if (chan != 0xFF) break;
		if (flag == 0x80) return chan;

		flag = 0x80;
	}

	/* Sustain Off */
	MPU_Send(0xB0 | chan, 64, 0);

	MPU_FlushChannel(chan);

	s_mpu_noteOnCount[chan] = 0;
	s_mpu_lockStatus[chan] |= 0x80;

	return chan;
}

static void MPU_289D(uint8 chan)
{
	if ((s_mpu_lockStatus[chan] & 0x80) == 0) return;

	s_mpu_lockStatus[chan] &= 0x7F;
	s_mpu_noteOnCount[chan] = 0;

	/* Sustain Off */
	MPU_Send(0xB0 | chan, 64, 0);

	/* All Notes Off */
	MPU_Send(0xB0 | chan, 123, 0);

	if (s_mpu_controls[chan].volume != 0xFF)      MPU_Send(0xB0 | chan,   7, s_mpu_controls[chan].volume);
	if (s_mpu_controls[chan].modulation != 0xFF)  MPU_Send(0xB0 | chan,   1, s_mpu_controls[chan].modulation);
	if (s_mpu_controls[chan].panpot != 0xFF) MPU_Send(0xB0 | chan,  10, s_mpu_controls[chan].panpot);
	if (s_mpu_controls[chan].expression != 0xFF) MPU_Send(0xB0 | chan,  11, s_mpu_controls[chan].expression);
	if (s_mpu_controls[chan].sustain != 0xFF)     MPU_Send(0xB0 | chan,  64, s_mpu_controls[chan].sustain);
	if (s_mpu_controls[chan].patch_bank_sel != 0xFF) MPU_Send(0xB0 | chan, 114, s_mpu_controls[chan].patch_bank_sel);
	if (s_mpu_controls[chan].chan_lock != 0xFF) MPU_Send(0xB0 | chan, 110, s_mpu_controls[chan].chan_lock);
	if (s_mpu_controls[chan].chan_protect != 0xFF) MPU_Send(0xB0 | chan, 111, s_mpu_controls[chan].chan_protect);
	if (s_mpu_controls[chan].voice_protect != 0xFF) MPU_Send(0xB0 | chan, 112, s_mpu_controls[chan].voice_protect);

	if (s_mpu_programs[chan] != 0xFF) MPU_Send(0xC0 | chan, s_mpu_programs[chan], 0);

	if (s_mpu_pitchWheel[chan] != 0xFFFF) MPU_Send(0xE0 | chan, s_mpu_pitchWheel[chan] & 0xFF, s_mpu_pitchWheel[chan] >> 8);
}

/* XMIDI.ASM - XMIDI_control procedure */
static void MPU_Control(MSData *data, uint8 chan, uint8 control, uint8 value)
{
	switch (control) {
		case 1:	/* MODULATION */
			s_mpu_controls[chan].modulation = value;
			data->controls[chan].modulation = value;
			if ((s_mpu_lockStatus[chan] & 0x80) == 0) MPU_Send(0xB0 | data->chanMaps[chan], control, value);
			break;

		case 7: /* PART_VOLUME / Channel Volume */
			if (data->variable_0024 == 100) break;
			value = min(data->variable_0024 * value / 100, 127);
			s_mpu_controls[chan].volume = value;
			if ((s_mpu_lockStatus[chan] & 0x80) == 0) MPU_Send(0xB0 | data->chanMaps[chan], control, value);
			break;

		case 10:	/* PANPOT */
			s_mpu_controls[chan].panpot = value;
			data->controls[chan].panpot = value;
			if ((s_mpu_lockStatus[chan] & 0x80) == 0) MPU_Send(0xB0 | data->chanMaps[chan], control, value);
			break;

		case 11:	/* EXPRESSION */
			s_mpu_controls[chan].expression = value;
			data->controls[chan].expression = value;
			if ((s_mpu_lockStatus[chan] & 0x80) == 0) MPU_Send(0xB0 | data->chanMaps[chan], control, value);
			break;

		case 64:	/* SUSTAIN */
			s_mpu_controls[chan].sustain = value;
			data->controls[chan].sustain = value;
			if ((s_mpu_lockStatus[chan] & 0x80) == 0) MPU_Send(0xB0 | data->chanMaps[chan], control, value);
			break;

		case 110:	/* CHAN_LOCK */
			s_mpu_controls[chan].chan_lock = value;
			data->controls[chan].chan_lock = value;
			if (value < 64) {
				MPU_FlushChannel(chan);
				MPU_289D(data->chanMaps[chan]);
				data->chanMaps[chan] = chan;
			} else {
				uint8 newChan = MPU_281A();
				if (newChan == 0xFF) newChan = chan;

				data->chanMaps[chan] = newChan;
			}
			break;

		case 111:	/* CHAN_PROTECT */
			s_mpu_controls[chan].chan_protect = value;
			data->controls[chan].chan_protect = value;
			if (value >= 64) s_mpu_lockStatus[chan] |= 0x40;
			break;

		case 112:	/* VOICE_PROTECT */
			s_mpu_controls[chan].voice_protect = value;
			data->controls[chan].voice_protect = value;
			if ((s_mpu_lockStatus[chan] & 0x80) == 0) MPU_Send(0xB0 | data->chanMaps[chan], control, value);
			break;

		case 114:	/* PATCH_BANK_SEL */
			s_mpu_controls[chan].patch_bank_sel = value;
			data->controls[chan].patch_bank_sel = value;
			if ((s_mpu_lockStatus[chan] & 0x80) == 0) MPU_Send(0xB0 | data->chanMaps[chan], control, value);
			break;

		case 115:	/* INDIRECT_C_PFX */
			assert(0); /* Not decompiled code */

		case 116: {	/* FOR_LOOP */
			uint8 i;

			for (i = 0; i < 4; i++) {
				if (data->variable_0060[i] == 0xFFFF) {
					data->variable_0060[i] = value;
					data->variable_0050[i] = data->sound;
					break;
				}
			}
		} break;

		case 117: {	/* NEXT_LOOP */
			uint8 i;

			if (value < 64) break;

			for (i = 0; i < 4; i++) {
				if (data->variable_0060[3 - i] != 0xFFFF) {
					if (data->variable_0060[3 - i] != 0 && --data->variable_0060[3 - i] == 0) {
						data->variable_0060[3 - i] = 0xFFFF;
						break;
					}
					data->sound = data->variable_0050[3 - i];
					break;
				}
			}
		} break;

		case 118:	/* CLEAR_BEAT_BAR */
		case 119:	/* CALLBACK_TRIG */
			assert(0); /* Not decompiled code */

		default:
			Debug("MPU_Control() %02X %02X %02X   control=%u\n", data->sound[0], control, value, control);
			if ((s_mpu_lockStatus[chan] & 0x80) == 0) MPU_Send(0xB0 | data->chanMaps[chan], control, value);
			break;
	}
}

static void MPU_16B7(MSData *data)
{
	uint8 chan;

	for (chan = 0; chan < 16; chan++) {
		if (data->controls[chan].sustain != 0xFF && data->controls[chan].sustain >= 64) {
			s_mpu_controls[chan].sustain = 0;
			/* Sustain Off */
			MPU_Send(0xB0 | chan, 64, 0);
		}

		if (data->controls[chan].chan_lock != 0xFF && data->controls[chan].chan_lock >= 64) {
			MPU_FlushChannel(chan);
			MPU_289D(data->chanMaps[chan]);
			data->chanMaps[chan] = chan;
		}

		if (data->controls[chan].chan_protect != 0xFF && data->controls[chan].chan_protect >= 64) s_mpu_lockStatus[chan] &= 0xBF;

		if (data->controls[chan].voice_protect != 0xFF && data->controls[chan].voice_protect >= 64) MPU_Send(0xB0 | chan, 112, 0);
	}
}

static uint16 MPU_XMIDIMeta(MSData *data)
{
	uint8 *sound;
	uint8 type;
	uint16 len = 0;

	sound = data->sound;
	type = sound[1];
	sound += 2;

	while (true) {
		uint8 v = *sound++;
		len |= v & 0x7F;
		if ((v & 0x80) == 0) break;
		len <<= 7;
	}
	len += (uint16)(sound - data->sound);

	switch (type) {
		case 0x2F:	/* End of track */
			MPU_16B7(data);

			data->playing = 2;
			if (data->delayedClear) MPU_ClearData(s_mpu_msdataCurrent);
			break;

		case 0x58: {	/* time sig */
			int8 mul;

			data->variable_0042 = sound[0];
			mul = (int8)sound[1] - 2;

			if (mul < 0) {
				data->variable_0044 = 133333 >> -mul;
			} else {
				data->variable_0044 = 133333 << mul;
			}

			data->variable_0048 = data->variable_0044;
		} break;

		case 0x51:	/* TEMPO meta-event */
			data->variable_004C = (sound[0] << 20) | (sound[1] << 12) | (sound[2] << 4);
			break;

		default:
			{
				int i;
				Warning("MPU_1B48() type=%02X len=%hu\n", (int)type, len);
				Warning("  ignored data : ");
				for(i = 0 ; i < len; i++) Warning(" %02X", data->sound[i]);
				Warning("\n");
			}
			break;
	}

	return len;
}

void MPU_Interrupt(void)
{
	static bool locked = false;
	uint16 count;

	if (s_mpuIgnore) return;
	if (locked) return;
	locked = true;

	s_mpu_msdataCurrent = -1;
	count = s_mpu_msdataSize;
	while (count-- != 0) {
		uint16 index;
		MSData *data;

		do {
			s_mpu_msdataCurrent++;
			index = s_mpu_msdataCurrent;
		} while (index < lengthof(s_mpu_msdata) && s_mpu_msdata[index] == NULL);

		if (index >= lengthof(s_mpu_msdata)) break;

		data = s_mpu_msdata[index];

		if (data->playing != 1) continue;

		data->variable_0030 += data->variable_0032;

		while (data->variable_0030 >= 0x64) {
			uint32 value;

			data->variable_0030 -= 0x64;

			value = data->variable_0048;
			value += data->variable_0044;

			if ((int32)value >= (int32)data->variable_004C) {
				value -= data->variable_004C;
				if (++data->variable_003E >= data->variable_0042) {
					data->variable_003E = 0x0;
					data->variable_0040++;
				}
			}

			data->variable_0048 = value;

			/* Handle note length */
			index = -1;
			while (data->noteOnCount != 0) {
				uint16 chan;
				uint8 note;
				while (++index < 0x20) {
					if (data->noteOnChans[index] != 0xFF) break;
				}
				if (index == 0x20) break;

				if (--data->noteOnDuration[index] >= 0) continue;

				chan = data->chanMaps[data->noteOnChans[index]];
				data->noteOnChans[index] = 0xFF;
				note = data->noteOnNotes[index];
				s_mpu_noteOnCount[chan]--;

				/* Note Off */
				MPU_Send(0x80 | chan, note, 0);

				data->noteOnCount--;
			}

			if (--data->delay <= 0) {
				do {
					uint8 status;
					uint8 chan;
					uint8 data1;
					uint8 data2;
					uint16 nb;

					status = data->sound[0];

					if (status < 0x80) {
						/* Set a delay before next command. */
						data->sound++;
						data->delay = status;
						break;
					}

					chan = status & 0xF;
					data1 = data->sound[1];
					data2 = data->sound[2];

					switch(status & 0xF0) {
					case 0xF0:	/* System */
						if(chan == 0xF) {
							/* 0xFF Meta event */
							nb = MPU_XMIDIMeta(data);
						} else if(chan == 0) {
							/* System Exclusive */
							static uint8 buffer[320];
							int i;
							/* decode XMID variable len */
							i = 1;
							nb = 0;
							do {
								nb = (nb << 7) | (data->sound[i] & 0x7F);
							} while(data->sound[i++] & 0x80);
							buffer[0] = status;
							assert(nb < sizeof(buffer));
							memcpy(buffer + 1, data->sound + i, nb);
							midi_send_string(buffer, nb + 1);
							nb += i;
						} else {
							Error("status = %02X\n", status);
							nb = 1;
						}
						break;
					case 0xE0:	/* Pitch Bend change */
						s_mpu_pitchWheel[chan] = (data2 << 8) + data1;
						if ((s_mpu_lockStatus[chan] & 0x80) == 0) {
							MPU_Send(status | data->chanMaps[chan], data1, data2);
						}
						nb = 0x3;
						break;
					case 0xD0:	/* Channel Pressure / aftertouch */
						if ((s_mpu_lockStatus[chan] & 0x80) == 0) {
							MPU_Send(status | data->chanMaps[chan], data1, data2);
						}
						nb = 0x2;
						break;
					case 0xC0:	/* Program Change */
						s_mpu_programs[chan] = data1;
						if ((s_mpu_lockStatus[chan] & 0x80) == 0) {
							MPU_Send(status | data->chanMaps[chan], data1, data2);
						}
						nb = 0x2;
						break;
					case 0xB0:	/* Control Change */
						MPU_Control(data, chan, data1, data2);
						nb = 0x3;
						break;
					case 0xA0:	/* Polyphonic key pressure / aftertouch */
						if ((s_mpu_lockStatus[chan] & 0x80) == 0) {
							MPU_Send(status | data->chanMaps[chan], data1, data2);
						}
						nb = 0x3;
						break;
					default:	/* 0x80 Note Off / 0x90 Note On */
						nb = MPU_NoteOn(data);
					}

					data->sound += nb;
				} while (data->playing == 1);
			}
			if (data->playing != 1) break;
		}

		if (data->playing != 1) continue;

		if (data->variable_0032 != data->variable_0034) {
			uint32 v;
			uint16 i;

			v = data->variable_0036;
			v += 0x53;
			i = 0xFFFF;

			do {
				i++;
				data->variable_0036 = v;
				v -= data->variable_003A;
			} while ((int32)v >= 0);

			if (i != 0){
				if (data->variable_0032 >= data->variable_0034) {
					data->variable_0032 = max(data->variable_0032 - i, data->variable_0034);
				} else {
					data->variable_0032 = min(data->variable_0032 + i, data->variable_0034);
				}
			}
		}

		if (data->variable_0024 != data->variable_0026) {
			uint32 v;
			uint16 i;

			v = data->variable_0028;
			v += 0x53;
			i = 0xFFFF;

			do {
				i++;
				data->variable_0028 = v;
				v -= data->variable_002C;
			} while ((int32)v >= 0);

			if (i != 0){
				if (data->variable_0024 >= data->variable_0026) {
					data->variable_0024 = max(data->variable_0024 - i, data->variable_0026);
				} else {
					data->variable_0024 = min(data->variable_0024 + i, data->variable_0026);
				}
				MPU_ApplyVolume(data);
			}
		}
	}

	locked = false;

	return;
}

static void *MPU_FindSoundStart(uint8 *file, uint16 index)
{
	uint32 total;
	uint32 header;
	uint32 size;

	index++;

	while (true) {
		header = BETOH32(*(uint32 *)(file + 0));
		size   = BETOH32(*(uint32 *)(file + 4));

		if (header != CC_CAT && header != CC_FORM) return NULL;
		if (*(uint32 *)(file + 8) == BETOH32(CC_XMID)) break;

		file += 8;
		file += size;
	}
	total = size - 5;

	if (header == CC_FORM) return (index == 1) ? file : NULL;

	file += 12;

	while (true) {
		size = BETOH32(*(uint32 *)(file + 4));

		if (*(uint32 *)(file + 8) == BETOH32(CC_XMID) && --index == 0) break;

		size = size + 8;
		total -= size;
		if ((int32)total < 0) return NULL;

		file += size;
	}

	return file;
}

static void MPU_InitData(MSData *data)
{
	uint8 i;

	for (i = 0; i < 4; i++) data->variable_0060[i] = 0xFFFF;

	for (i = 0; i < 16; i++) {
		data->chanMaps[i] = i;
	}

	memset(data->controls, 0xFF, sizeof(data->controls));
	memset(data->noteOnChans, 0xFF, sizeof(data->noteOnChans));

	data->delay = 0;
	data->noteOnCount = 0;
	data->variable_0024 = 0x5A;
	data->variable_0026 = 0x5A;
	data->variable_0030 = 0;
	data->variable_0032 = 0x64;
	data->variable_0034 = 0x64;
	data->variable_003E = 0;
	data->variable_0040 = 0;
	data->variable_0042 = 4;
	data->variable_0044 = 0x208D5;
	data->variable_0048 = 0x208D5;
	data->variable_004C = 0x7A1200;
}

uint16 MPU_SetData(uint8 *file, uint16 index, void *msdata)
{
	MSData *data = msdata;
	uint32 header;
	uint32 size;
	uint16 i;

	if (file == NULL) return 0xFFFF;

	for (i = 0; i < 8; i++) {
		if (s_mpu_msdata[i] == NULL) break;
	}
	if (i == 8) return 0xFFFF;

	file = MPU_FindSoundStart(file, index);
	if (file == NULL) return 0xFFFF;

	s_mpu_msdata[i] = data;
	data->EVNT = NULL;

	header = BETOH32(*(uint32 *)(file + 0));
	size   = 12;
	while (header != CC_EVNT) {
		file += size;
		header = BETOH32(*(uint32 *)(file + 0));
		size   = BETOH32(*(uint32 *)(file + 4)) + 8;
	}

	data->EVNT = file;
	data->playing = 0;
	data->delayedClear = false;

	s_mpu_msdataSize++;

	MPU_InitData(data);

	return i;
}

void MPU_Play(uint16 index)
{
	MSData *data;

	if (index == 0xFFFF) return;

	data = s_mpu_msdata[index];

	if (data->playing == 1) MPU_Stop(index);

	MPU_InitData(data);

	data->sound = data->EVNT + 8;

	data->playing = 1;
}

static void MPU_StopAllNotes(MSData *data)
{
	uint8 i;

	for (i = 0; i < 32; i++) {
		uint8 note;
		uint8 chan;

		chan = data->noteOnChans[i];
		if (chan == 0xFF) continue;

		data->noteOnChans[i] = 0xFF;
		note = data->noteOnNotes[i];
		chan = data->chanMaps[chan];

		/* Note Off */
		MPU_Send(0x80 | chan, note, 0);
	}

	data->noteOnCount = 0;
}

void MPU_Stop(uint16 index)
{
	MSData *data;

	if (index == 0xFFFF) return;
	if (s_mpu_msdata[index] == NULL) return;

	data = s_mpu_msdata[index];

	if (data->playing != 1) return;

	MPU_StopAllNotes(data);
	MPU_16B7(data);

	data->playing = 0;
}

uint16 MPU_IsPlaying(uint16 index)
{
	MSData *data;

	if (index == 0xFFFF) return 0xFFFF;

	data = s_mpu_msdata[index];

	return data->playing;
}

uint16 MPU_GetDataSize(void)
{
	return sizeof(MSData);
}

#if defined(_WIN32)
static ThreadStatus WINAPI MPU_ThreadProc(void *data)
{
	VARIABLE_NOT_USED(data);
	Semaphore_Lock(s_mpu_sem);
	while (!Semaphore_TryLock(s_mpu_sem)) {
		msleep(s_mpu_usec / 1000);
		MPU_Interrupt();
	}
	Semaphore_Unlock(s_mpu_sem);
	return 0;
}
#endif /* _WIN32 */

bool MPU_Init(void)
{
	uint8 i;

	if (!midi_init()) return false;

#if defined(_WIN32)
	s_mpu_sem = Semaphore_Create(0);
	if (s_mpu_sem == NULL) {
		Error("Failed to create semaphore\n");
		return false;
	}

	s_mpu_thread = Thread_Create(MPU_ThreadProc, NULL);
	if (s_mpu_thread == NULL) {
		Error("Failed to create thread\n");
		Semaphore_Destroy(s_mpu_sem);
		return false;
	}
#endif /* _WIN32 */

	s_mpu_msdataSize = 0;
	s_mpu_msdataCurrent = 0;
	memset(s_mpu_msdata, 0, sizeof(s_mpu_msdata));

	memset(s_mpu_controls,   0xFF, sizeof(s_mpu_controls));
	memset(s_mpu_programs,   0xFF, sizeof(s_mpu_programs));
	memset(s_mpu_pitchWheel, 0xFF, sizeof(s_mpu_pitchWheel));

	memset(s_mpu_noteOnCount, 0, sizeof(s_mpu_noteOnCount));
	memset(s_mpu_lockStatus, 0, sizeof(s_mpu_lockStatus));

	s_mpuIgnore = true;
	midi_reset();
	s_mpuIgnore = false;

	for (i = 0; i < 9; i++) {
		static const uint8 defaultPrograms[9] = { 68, 48, 95, 78, 41, 3, 110, 122, 255 };
		uint8 chan = i + 1;

		s_mpu_controls[chan].volume = 127;
		s_mpu_controls[chan].modulation = 0;
		s_mpu_controls[chan].panpot = 64;
		s_mpu_controls[chan].expression = 127;
		s_mpu_controls[chan].sustain = 0;
		s_mpu_controls[chan].patch_bank_sel = 0;
		s_mpu_controls[chan].chan_lock = 0;
		s_mpu_controls[chan].chan_protect = 0;
		s_mpu_controls[chan].voice_protect = 0;

		MPU_Send(0xB0 | chan,   7, s_mpu_controls[chan].volume);        /* 7 = PART_VOLUME */
		MPU_Send(0xB0 | chan,   1, s_mpu_controls[chan].modulation);    /* 1 = MODULATION */
		MPU_Send(0xB0 | chan,  10, s_mpu_controls[chan].panpot);        /* 10 = PANPOT */
		MPU_Send(0xB0 | chan,  11, s_mpu_controls[chan].expression);    /* 11 = EXPRESSION */
		MPU_Send(0xB0 | chan,  64, s_mpu_controls[chan].sustain);       /* 64 = SUSTAIN */
		MPU_Send(0xB0 | chan, 114, s_mpu_controls[chan].patch_bank_sel);/* 114 = PATCH_BANK_SEL */
		MPU_Send(0xB0 | chan, 110, s_mpu_controls[chan].chan_lock);     /* 110 = CHAN_LOCK */
		MPU_Send(0xB0 | chan, 111, s_mpu_controls[chan].chan_protect);  /* 111 = CHAN_PROTECT */
		MPU_Send(0xB0 | chan, 112, s_mpu_controls[chan].voice_protect); /* 112 = VOICE_PROTECT */

		s_mpu_pitchWheel[chan] = 0x4000;
		MPU_Send(0xE0 | chan, 0x00, 0x40);	/* 0xE0 : Pitch Bend change */

		if (defaultPrograms[i] == 0xFF) continue;
		s_mpu_programs[chan] = defaultPrograms[i];
		MPU_Send(0xC0 | chan, 0, defaultPrograms[i]);	/* 0xC0 : program change */
	}

	s_mpu_initialized = true;
	return true;
}

void MPU_Uninit(void)
{
	uint16 i;

	if (!s_mpu_initialized) return;

	for (i = 0; i < s_mpu_msdataSize; i++) {
		if (s_mpu_msdata[i] == NULL) continue;
		MPU_Stop(i);
		MPU_ClearData(i);
	}

	s_mpuIgnore = true;
	midi_reset();

	s_mpu_initialized = false;

	midi_uninit();
	s_mpuIgnore = false;

#if defined(_WIN32)
	Semaphore_Destroy(s_mpu_sem);
#endif /* _WIN32 */
}

void MPU_ClearData(uint16 index)
{
	MSData *data;

	if (index == 0xFFFF) return;
	if (s_mpu_msdata[index] == NULL) return;

	data = s_mpu_msdata[index];

	if (data->playing == 1) {
		data->delayedClear = true;
	} else {
		s_mpu_msdata[index] = NULL;
		s_mpu_msdataSize--;
	}
}

void MPU_SetVolume(uint16 index, uint16 volume, uint16 arg0C)
{
	MSData *data;
	uint16 diff;

	if (index == 0xFFFF) return;

	data = s_mpu_msdata[index];

	data->variable_0026 = volume;

	if (arg0C == 0) {
		data->variable_0024 = volume;
		MPU_ApplyVolume(data);
		return;
	}

	diff = data->variable_0026 - data->variable_0024;
	if (diff == 0) return;

	data->variable_002C = 10 * arg0C / abs(diff);
	if (data->variable_002C == 0) data->variable_002C = 1;
	data->variable_0028 = 0;
}

#if defined(_WIN32)
void MPU_StartThread(uint32 usec)
{
	s_mpu_usec = usec;
	Semaphore_Unlock(s_mpu_sem);
}

void MPU_StopThread(void)
{
	Semaphore_Unlock(s_mpu_sem);
	Thread_Wait(s_mpu_thread, NULL);
}
#endif /* _WIN32 */
