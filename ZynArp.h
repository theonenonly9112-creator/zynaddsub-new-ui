/*
  ZYNNM XT - ZynArp.h - arpeggiator

  The note logic (chord list, Up / Down / Ping-pong stepping, octave repeats, retrigger and
  pressure-to-velocity handling) is ported from the Arpeggiator of Bespoke Synth:
      Copyright (C) 2021 Ryan Challinor (contact: awwbees@gmail.com)
      GNU General Public License, version 3 or (at your option) any later version.
  Everything that belongs to the Bespoke / OpenFrameworks environment (module and drawing code, GUI
  controls, the transport and the scale object) is removed. What remains is plain C++11.

  Usage in the audio loop (all calls from the same thread, normally the audio thread):

      ZynArp arp;
      arp.setSink(mySink, this);            // or read the events with arp.popEvent()
      arp.setEnabled(true);
      ...
      if(!arp.playNote(note, velocity))     // velocity 0 = note off. true = the arpeggiator took it
          engine.noteOn/noteOff(...);       // false = pass the event on unchanged
      ...
      arp.setBpm(hostBpm);                  // BPM of the host (or your own tempo when standalone)
      arp.tick(buffersize / samplerate);    // once per audio buffer, deltaTime in seconds

  Nothing allocates after the constructor: the chord vector reserves 128 entries (there can never be
  more, a pitch is only in the chord once) and the output queue is a fixed array.
*/

#ifndef ZYNARP_H
#define ZYNARP_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace zyn {

class ZynArp
{
    public:
        /** Length of one arpeggiator step (names as in Bespoke: n = note, t = triplet). */
        enum Interval
        {
            kInterval_1n,   // 4    beats
            kInterval_2n,   // 2
            kInterval_4n,   // 1
            kInterval_4nt,  // 2/3
            kInterval_8n,   // 1/2
            kInterval_8nt,  // 1/3
            kInterval_16n,  // 1/4
            kInterval_16nt, // 1/6
            kInterval_32n,  // 1/8
            kInterval_64n,  // 1/16
            kNumIntervals
        };

        /** A standard MIDI channel message. */
        struct MidiEvent
        {
            uint8_t status;     //!< 0x90 | channel = note on, 0x80 | channel = note off
            uint8_t note;       //!< 0..127
            uint8_t velocity;   //!< note on: 1..127, note off: 0
            double  offset;     //!< seconds after the start of the tick() call that created it
            bool    isNoteOn() const { return (status & 0xF0) == 0x90; }
            int     channel() const { return status & 0x0F; }
        };

        /** Optional direct output. Without a sink the events wait in the queue (popEvent). */
        typedef void (*MidiSink)(void *user, const MidiEvent &ev);

        ZynArp()
        {
            mChord.reserve(128);
            mInputNotes.fill(false);
            for(int i = 0; i < kQueueSize; ++i) {
                mQueue[i].status   = 0;
                mQueue[i].note     = 0;
                mQueue[i].velocity = 0;
                mQueue[i].offset   = 0.0;
            }
        }

        // ------------------------------------------------------------------ settings
        void setSink(MidiSink sink, void *user)
        {
            mSink     = sink;
            mSinkUser = user;
        }

        void setEnabled(bool enabled)
        {
            if(enabled == mEnabled)
                return;
            Guard g(mLock);
            flushLocked();                 // a note that is still sounding is turned off
            mEnabled = enabled;
        }

        bool enabled() const { return mEnabled; }

        void setBpm(double bpm) { mBpm = (bpm > 1.0) ? bpm : 120.0; }
        void setInterval(Interval interval)
        {
            if(interval >= 0 && interval < kNumIntervals)
                mInterval = interval;
        }
        void setChannel(int channel) { mChannel = channel & 0x0F; }

        /** -3..3. 1 = up, -1 = down, 2 / 3 = skip notes, 0 = ping-pong. */
        void setStep(int step)
        {
            Guard g(mLock);
            if(step < -3) step = -3;
            if(step > 3)  step = 3;
            if(mArpStep > 0)       // same as Bespoke's IntSliderUpdated: remember the direction for ping-pong
                mArpPingPongDirection = 1;
            else if(mArpStep < 0)
                mArpPingPongDirection = -1;
            mArpStep = step;
        }

        /** 1..4. How many octaves the pattern climbs before it starts again. */
        void setOctaveRepeats(int repeats)
        {
            Guard g(mLock);
            if(repeats < 1) repeats = 1;
            if(repeats > 4) repeats = 4;
            mOctaveRepeats       = repeats;
            mCurrentOctaveOffset %= repeats;
        }

        Interval interval() const { return mInterval; }
        int      step() const { return mArpStep; }
        int      octaveRepeats() const { return mOctaveRepeats; }
        double   bpm() const { return mBpm; }

        // ------------------------------------------------------------------ notes in
        /**
         * Bespoke's PlayNote. velocity > 0 adds the note to the chord, velocity 0 removes it.
         * @return true when the arpeggiator took the event (do not play it yourself). false when the
         *         event is not for the arpeggiator (it is off, the pitch is out of range, or it is the
         *         note off of a key that was pressed before the arpeggiator was switched on): play it
         *         as usual so that no note can hang.
         */
        bool playNote(int pitch, int velocity)
        {
            if(!mEnabled || pitch < 0 || pitch >= 128)
                return false;

            Guard g(mLock);
            if(velocity > 0) {
                if(!mInputNotes[pitch]) {
                    if(mChord.empty())
                        restartLocked();           // first key: start the pattern at once
                    mChord.push_back(ArpNote(pitch, velocity));
                    mInputNotes[pitch] = true;
                }
                return true;
            }

            if(!mInputNotes[pitch])
                return false;
            for(std::vector<ArpNote>::iterator it = mChord.begin(); it != mChord.end(); ++it) {
                if(it->mPitch == pitch) {
                    mChord.erase(it);
                    break;
                }
            }
            mInputNotes[pitch] = false;
            return true;
        }

        bool noteOn(int pitch, int velocity) { return playNote(pitch, velocity > 0 ? velocity : 1); }
        bool noteOff(int pitch) { return playNote(pitch, 0); }

        /** Polyphonic aftertouch (0..127) of a held key. Bespoke's "pressure controlled velocity". */
        void setPressure(int pitch, int pressure)
        {
            if(pitch < 0 || pitch >= 128)
                return;
            Guard g(mLock);
            for(size_t i = 0; i < mChord.size(); ++i)
                if(mChord[i].mPitch == pitch)
                    mChord[i].mPressure = pressure < 0 ? 0 : (pressure > 127 ? 127 : pressure);
        }

        // ------------------------------------------------------------------ the clock
        /**
         * Call once per audio buffer. deltaTime = length of the buffer in seconds. Every step that
         * falls inside the buffer plays (one step per beat fraction of the interval, at the BPM set
         * with setBpm). The events carry their position inside the buffer in MidiEvent::offset.
         */
        void tick(double deltaTime)
        {
            if(!mEnabled || deltaTime <= 0.0)
                return;

            Guard g(mLock);
            const double beatsPerSecond = mBpm / 60.0;
            const double stepBeats      = intervalBeats(mInterval);
            double       beatsLeft      = deltaTime * beatsPerSecond;   // beats this buffer covers
            double       elapsed        = 0.0;                          // seconds since the start of the buffer

            // a step that falls exactly on the end of the buffer belongs to the next buffer (offset 0)
            for(int guardCount = 0; guardCount < 64 && mBeatsToNext < beatsLeft; ++guardCount) {
                beatsLeft -= mBeatsToNext;
                elapsed   += mBeatsToNext / beatsPerSecond;
                onTimeEvent(elapsed);
                mBeatsToNext = stepBeats;
            }
            mBeatsToNext -= beatsLeft;
            if(mBeatsToNext < 0.0)
                mBeatsToNext = stepBeats;     // only reached when the guard stopped a runaway loop
        }

        /** Next queued event (only used without a sink). Returns false when the queue is empty. */
        bool popEvent(MidiEvent &ev)
        {
            if(mQueueRead == mQueueWrite)
                return false;
            ev         = mQueue[mQueueRead];
            mQueueRead = (mQueueRead + 1) % kQueueSize;
            return true;
        }

        /** Stop the pattern: turn off the sounding note, forget all held keys. */
        void allNotesOff()
        {
            Guard g(mLock);
            flushLocked();
        }

        // ------------------------------------------------------------------ for a display
        int chordSize() const { return (int)mChord.size(); }
        int chordPitch(int i) const { return (i >= 0 && i < (int)mChord.size()) ? mChord[i].mPitch : -1; }
        int currentIndex() const { return mArpIndex; }
        int lastPitch() const { return mLastPitch; }

    private:
        static const int kPitchesPerOctave = 12;     // TheScale->GetPitchesPerOctave() of Bespoke
        static const int kQueueSize        = 256;

        struct ArpNote
        {
            ArpNote(int pitch, int vel)
                : mPitch(pitch), mInitialVelocity(vel), mPressure(0), mPressureControlledVelocity(false) {}
            int  mPitch;
            int  mInitialVelocity;
            int  mPressure;                   // polyphonic aftertouch, 0 = none
            bool mPressureControlledVelocity;
        };

        // a tiny lock: the sections are a few instructions long, and when everything runs on the audio
        // thread it is never contended
        struct Guard
        {
            explicit Guard(std::atomic_flag &f) : mFlag(f)
            {
                while(mFlag.test_and_set(std::memory_order_acquire)) {}
            }
            ~Guard() { mFlag.clear(std::memory_order_release); }
            std::atomic_flag &mFlag;
        private:
            Guard(const Guard &);
            Guard &operator=(const Guard &);
        };

        static double intervalBeats(Interval i)
        {
            switch(i) {
                case kInterval_1n:   return 4.0;
                case kInterval_2n:   return 2.0;
                case kInterval_4n:   return 1.0;
                case kInterval_4nt:  return 2.0 / 3.0;
                case kInterval_8n:   return 0.5;
                case kInterval_8nt:  return 1.0 / 3.0;
                case kInterval_16n:  return 0.25;
                case kInterval_16nt: return 1.0 / 6.0;
                case kInterval_32n:  return 0.125;
                case kInterval_64n:  return 0.0625;
                default:             return 0.25;
            }
        }

        void emit(double offset, int pitch, int velocity)
        {
            MidiEvent ev;
            ev.status   = (uint8_t)((velocity > 0 ? 0x90 : 0x80) | mChannel);
            ev.note     = (uint8_t)pitch;
            ev.velocity = (uint8_t)(velocity > 0 ? (velocity > 127 ? 127 : velocity) : 0);
            ev.offset   = offset;
            if(mSink) {
                mSink(mSinkUser, ev);
                return;
            }
            const int next = (mQueueWrite + 1) % kQueueSize;
            if(next == mQueueRead)
                return;                              // queue full: drop (the host stopped reading)
            mQueue[mQueueWrite] = ev;
            mQueueWrite         = next;
        }

        // the pattern starts again from the first note
        void restartLocked()
        {
            if(mArpStep > 0)                         // the first step lands on the lowest note (going up),
                mArpIndex = -mArpStep;               // on the top one (going down)
            else if(mArpStep < 0)
                mArpIndex = 0;
            else
                mArpIndex = -1;                      // ping-pong
            mArpPingPongDirection = (mArpStep < 0) ? -1 : 1;
            mCurrentOctaveOffset  = 0;
            mBeatsToNext          = 0.0;                              // first step on the next tick
        }

        void flushLocked()
        {
            if(mLastPitch != -1)
                emit(0.0, mLastPitch, 0);
            mLastPitch = -1;
            mChord.clear();
            mInputNotes.fill(false);
            mArpIndex = -1;
            mArpPingPongDirection = 1;
            mCurrentOctaveOffset  = 0;
            mBeatsToNext          = 0.0;
        }

        // Bespoke's Arpeggiator::OnTimeEvent (the caller holds the lock)
        void onTimeEvent(double offset)
        {
            const int size = (int)mChord.size();
            if(size == 0) {
                if(mLastPitch != -1)
                    emit(offset, mLastPitch, 0);
                mLastPitch = -1;
                return;
            }

            if(mArpStep != 0) {
                mArpIndex += mArpStep;
                while(mArpIndex >= size) {
                    mArpIndex -= size;
                    mCurrentOctaveOffset = (mCurrentOctaveOffset + 1) % mOctaveRepeats;
                }
                while(mArpIndex < 0) {
                    mArpIndex += size;
                    mCurrentOctaveOffset = (mCurrentOctaveOffset - 1 + mOctaveRepeats) % mOctaveRepeats;
                }
            }
            else {   // ping-pong
                mArpIndex += mArpPingPongDirection;
                if(size >= 2) {
                    if(mArpIndex < 0) {
                        mArpIndex             = 1;
                        mArpPingPongDirection = 1;
                    }
                    if(mArpIndex > size - 1) {
                        mArpIndex             = size - 2;
                        mArpPingPongDirection = -1;
                    }
                }
                else
                    mArpIndex = mArpIndex < 0 ? 0 : (mArpIndex > size - 1 ? size - 1 : mArpIndex);
            }

            int offPitch = (mLastPitch >= 0) ? mLastPitch : -1;

            ArpNote &current  = mChord[mArpIndex];
            int      outPitch = current.mPitch + mCurrentOctaveOffset * kPitchesPerOctave;
            while(outPitch > 127)                    // MIDI ends at 127: fold back down by octaves
                outPitch -= kPitchesPerOctave;

            if(mLastPitch == outPitch) {             // same note again: note off first
                emit(offset, mLastPitch, 0);
                offPitch = -1;
            }

            int velocity = current.mInitialVelocity;
            const int pressure = current.mPressure;
            if(pressure >= current.mInitialVelocity)   // "release" the velocity to pressure control if we press harder than the strike
                current.mPressureControlledVelocity = true;
            if(pressure > 0 && current.mPressureControlledVelocity)
                velocity = pressure;
            emit(offset, outPitch, velocity);
            mLastPitch = outPitch;

            if(offPitch != -1) {                     // the previous note ends after the new one started
                emit(offset, offPitch, 0);
                if(offPitch == mLastPitch)
                    mLastPitch = -1;
            }
        }

        // ---- state (names as in Bespoke)
        std::vector<ArpNote>   mChord;
        std::array<bool, 128>  mInputNotes;
        std::atomic_flag       mLock = ATOMIC_FLAG_INIT;

        bool     mEnabled{false};
        Interval mInterval{kInterval_16n};
        int      mLastPitch{-1};
        int      mArpIndex{-1};
        int      mArpStep{1};
        int      mArpPingPongDirection{1};
        int      mCurrentOctaveOffset{0};
        int      mOctaveRepeats{1};

        // ---- clock and output
        double    mBpm{120.0};
        double    mBeatsToNext{0.0};
        int       mChannel{0};
        MidiSink  mSink{nullptr};
        void     *mSinkUser{nullptr};
        MidiEvent mQueue[kQueueSize];
        int       mQueueRead{0};
        int       mQueueWrite{0};
};

}

#endif
