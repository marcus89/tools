#include "MidiFile.h"
#include "Options.h"
#include <iostream>
#include <iomanip>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <unistd.h>
#include <cmath>

// g++ sequencer.cpp -o sequencer -I../midifile/include -L../midifile/lib -ljack -lmidifile

using namespace std;
using namespace smf;

#define SYNC_REQUESTED 1
#define PLAYING 2
#define STOPPED 3

MidiFile midifile;
jack_client_t *m_jack_client = nullptr;
jack_port_t *m_midi_output_port = nullptr;
int event_index = 0;
double m_sample_rate = 48000.0; // get from jack
int m_midi_state = STOPPED;
float m_time_beats_per_bar = 4.0;      // get from midi
float m_time_beat_type = 4.0;          // get from midi
double m_time_ticks_per_beat = 1920.0; // TPQ * 4
double m_time_beats_per_minute = 120.0;
int m_time_reset = 1; /* true when time values change */
double m_last_tick;
double m_current_bpm = 0.0;
uint64_t m_frame_per_bar = 0;
double m_ratio = 1.0;
jack_nframes_t m_offset = 0;
double frame_per_beat = 0.0;
double frame_per_tick = 0.0;

// JackClient::JackClient()
// {
//     m_midi_state = STOPPED;
// }

// int JackClient::stop()
// {
//     if (m_jack_client != NULL)
//     {
//         jack_transport_stop(m_jack_client);
//         jack_deactivate(m_jack_client);
//         jack_client_close(m_jack_client);
//     }
//     return 0;
// }

double nframes_to_seconds(jack_nframes_t nframes)
{
    return nframes / m_sample_rate;
}

double second_to_nframes(double sec)
{
    return sec * m_sample_rate;
}

// double JackClient::second_to_nframes(double sec)
// {
//     return sec * m_sample_rate;
// }

// smf_event_t *JackClient::get_next_event()
// {
//     smf_event_t *next_event = NULL;

//     while (next_event == NULL)
//     {
//         m_msg = smf_get_next_event(m_smf);
//         if (m_msg == NULL)
//         {
//             // smf_seek_to_seconds(smf, 0);
//             smf_rewind(m_smf);
//             continue;
//         }
//         if (smf_event_is_metadata(m_msg))
//         {
//             continue;
//         }
//         next_event = m_msg;
//     }
//     return next_event;
// }

void update_bpm(jack_position_t *pos)
{

    double new_bpm = pos->beats_per_minute;
    float beats_per_bar = pos->beats_per_bar;

    if (m_current_bpm != new_bpm)
    {
        double beat_per_second = new_bpm / 60.0;
        m_frame_per_bar = (uint64_t)((beats_per_bar / beat_per_second) * m_sample_rate);
        m_current_bpm = new_bpm;
        m_ratio = m_current_bpm / 120.0;
        m_midi_state = SYNC_REQUESTED;

        frame_per_beat = m_sample_rate * 60.0 / new_bpm;
        frame_per_tick = frame_per_beat / midifile.getTicksPerQuarterNote();
        printf("BPM: %f f_per_beat: %6f, f_per_tick: %6f\n", m_current_bpm, frame_per_beat, frame_per_tick);
    }
}

void seek_seconds(double sec)
{
    event_index = 0;
    int tracks = midifile.getTrackCount();
    for (int event = 0; event < midifile[0].size(); event++)
    {
        if (midifile[0][event].isMetaMessage())
            continue;

        if (midifile[0][event].seconds >= sec)
        {
            event_index = event;
            break;
        }
    }
    cout << "Event index for sec " << std::to_string(sec) << " is " << std::to_string(event_index) << endl;
}

void seek_midi(double jack_tick)
{
    event_index = 0;
    // convert jack tick to midi tick
    double jack_to_midi_tick = jack_tick * midifile.getTicksPerQuarterNote() / m_time_ticks_per_beat;

    printf("jack to midi ticks: %f\n", jack_to_midi_tick);
    int tracks = midifile.getTrackCount();
    for (int event = 0; event < midifile[1].size(); event++)
    {
        if (midifile[1][event].isMetaMessage())
            continue;

        if (midifile[1][event].tick >= jack_to_midi_tick)
        {
            event_index = event;
            break;
        }
    }
    cout << "Event index for tick " << std::to_string(jack_to_midi_tick) << " is " << std::to_string(event_index) << endl;
}

void seek_midi(jack_position_t pos)
{
    event_index = 0;

    // locate from jack BBT on current bar
    double current_jack_ticks = (pos.beat - 1) * pos.ticks_per_beat + pos.tick;

    // convert jack midi tick_per_beat -> midifile tick_per_beat
    double current_midi_ticks = current_jack_ticks * midifile.getTicksPerQuarterNote() / pos.ticks_per_beat;
    printf("Seeking jack: %f midi:%f\n", current_jack_ticks, current_midi_ticks);

    // // jack frame per tick
    // double jack_frame_per_tick = frame_per_beat / pos.ticks_per_beat;
    // double frame_of_bar_start = pos.frame - current_total_jack_ticks * jack_frame_per_tick;
    // printf("JACK : current bar: %d beat:%d tick: %d\n", pos.bar, pos.beat, pos.tick);
    // printf("JACK started at %f\n", frame_of_bar_start);

    // double jack_to_midi_ticks = current_total_jack_ticks * midifile.getTicksPerQuarterNote() / pos.ticks_per_beat;
    for (int event = 0; event < midifile[1].size(); event++)
    {
        if (!midifile[1][event].isNoteOn())
            continue;

        if (midifile[1][event].tick >= current_midi_ticks)
        {
            event_index = event;
            break;
        }
    }

    // printf("bar: %d beat:%d tick:%d\n", pos.bar, pos.beat, pos.tick);
    printf("Finding event based on midi tick: %f => event at index: %d\n", current_jack_ticks, event_index);
}

bool init = false;
int process_on_beat(jack_nframes_t nframes, void *)
{

    jack_position_t pos;

    jack_transport_state_t state = jack_transport_query(m_jack_client, &pos);

    if (state != JackTransportRolling || !(pos.valid & JackPositionBBT))
    {
        return 0;
    }

    jack_midi_clear_buffer( jack_port_get_buffer(m_midi_output_port, nframes));

    void *buffer = jack_port_get_buffer(m_midi_output_port, nframes);

    if(!init){
        seek_midi(pos);
        init = true;
    }
    /*
     * Convert JACK BBT to tick position
     */

    double ticks_per_frame = (pos.beats_per_minute * pos.ticks_per_beat) / (pos.frame_rate * 60.0);
    double current_tick = ((double)(pos.bar - 1) * pos.beats_per_bar * pos.ticks_per_beat) + ((double)(pos.beat - 1) * pos.ticks_per_beat) + pos.tick_double;
    /*
     * Find next beat tick
     */

    double next_beat_tick = ceil(current_tick / pos.ticks_per_beat) * pos.ticks_per_beat;
    /*
     * Avoid triggering the current beat twice
     */

    if (fabs(next_beat_tick - current_tick) < 0.001)
    {
        printf("ok\n");
        next_beat_tick += pos.ticks_per_beat;
    }

    /*
     * Convert next beat tick to frame
     */

    double frames_until_beat = (next_beat_tick - current_tick) / ticks_per_frame;
    int64_t beat_frame = pos.frame + llround(frames_until_beat); // this is the offset!

    /*
     * Is the beat inside this JACK period?
     */

    int64_t offset = beat_frame - pos.frame;

    if (offset >= 0 && offset < nframes)
    {
        uint8_t note_on[3] =
            {
                0x90 + pos.beat % 4, // note on channel 1
                60,                  // C4
                100};

        jack_midi_event_write(
            buffer,
            offset,
            note_on,
            sizeof(note_on));

        printf("Beat %d tick:%f MIDI %d at frame=%ld offset=%ld\n",
               pos.beat, pos.tick_double, (pos.beat % 4) + 1, beat_frame,
               offset);
    }

    return 0;
}

// int process_callback(jack_nframes_t nframes, void *arg)
// {

//     if (m_midi_state == STOPPED)
//     {
//         return 0;
//     }

//     void *port_buf = jack_port_get_buffer(m_midi_output_port, nframes);
//     unsigned char *buffer;
//     jack_midi_clear_buffer(port_buf);

//     jack_transport_state_t transport_state;
//     jack_position_t transport_pos;
//     transport_state = jack_transport_query(m_jack_client, &transport_pos);

//     if (transport_state != JackTransportRolling)
//     {
//         return 0;
//     }

//     if (!(transport_pos.valid & JackPositionBBT))
//     {
//         return 0;
//     }

//     update_bpm(&transport_pos);

//     // get current frame position
//     jack_nframes_t frame_time = transport_pos.frame;

//     while (1)
//     {
//         // start the reading
//         if (m_midi_state == SYNC_REQUESTED)
//         {
//             seek_midi_nframes(transport_pos, frame_time);
//             m_midi_state = PLAYING;
//             printf("-------------\n");
//             // break;
//         }
//         // break;

//         MidiEvent &msg = midifile[1][event_index];
//         if (!msg.isNoteOn())
//         {
//             event_index++;
//             if (event_index >= midifile[1].getEventCount())
//             {
//                 event_index = 0;
//             }
//             continue;
//         }
//         double event_frame = msg.tick * frame_per_tick;
//         double f = frame_time % m_frame_per_bar;

//         m_offset = abs(event_frame - f);
//         if (m_offset + nframes > m_frame_per_bar)
//         {
//             m_offset = m_frame_per_bar - m_offset;
//         }

//         if (m_offset >= nframes)
//         {
//             return 0;
//         }

//         msg.setChannel(event_index / 2);
//         printf("Sending event %d on bar:%d beat: %d tick:%d foffset %d\n", event_index, transport_pos.bar, transport_pos.beat, transport_pos.tick, m_offset);
//         jack_midi_event_write(port_buf, (jack_nframes_t)m_offset, msg.data(), msg.size());

//         event_index++;
//         if (event_index >= midifile[1].getEventCount())
//         {
//             cout << "reset" << endl;
//             event_index = 0;
//         }
//     }
//     return 0;
// }

// jack_nframes_t next_beat_frame = 0;
// bool initialized = false;

// int process2(jack_nframes_t nframes, void *)
// {
//     void *buffer = jack_port_get_buffer(m_midi_output_port, nframes);
//     jack_midi_clear_buffer(buffer);

//     jack_position_t pos;
//     jack_transport_query(m_jack_client, &pos);

//     if (!(pos.valid & JackPositionBBT))
//         return 0;

//     double frames_per_beat =
//         pos.frame_rate * 60.0 / pos.beats_per_minute;

//     // Initialize from current BBT position
//     if (!initialized)
//     {
//         double beat_position =
//             pos.tick / pos.ticks_per_beat;

//         next_beat_frame =
//             pos.frame +
//             (jack_nframes_t)((1.0 - beat_position) *
//                              frames_per_beat);

//         printf(
//             "JACK BBT init: bar=%d beat=%d tick=%.3f "
//             "frame=%u bpm=%.2f "
//             "frames_per_beat=%.2f "
//             "next_beat_frame=%u\n",
//             pos.bar,
//             pos.beat,
//             pos.tick,
//             pos.frame,
//             pos.beats_per_minute,
//             frames_per_beat,
//             next_beat_frame);
//         initialized = true;
//     }

//     jack_nframes_t start = pos.frame;
//     jack_nframes_t end = start + nframes;

//     while (next_beat_frame < end)
//     {
//         if (next_beat_frame >= start)
//         {
//             jack_nframes_t offset =
//                 next_beat_frame - start;

//             unsigned char msg[] =
//                 {
//                     0x90, // note on
//                     60,
//                     100};

//             jack_midi_event_write(
//                 buffer,
//                 offset,
//                 msg,
//                 3);
//         }

//         next_beat_frame +=
//             (jack_nframes_t)round(frames_per_beat);
//     }

//     return 0;
// }

// double midi_current_tick = 0.0;
// double midi_tpq = 192.0;

// int process3(jack_nframes_t nframes, void *)
// {
//     void *buffer = jack_port_get_buffer(m_midi_output_port, nframes);

//     jack_midi_clear_buffer(buffer);

//     jack_position_t pos;

//     jack_transport_state_t state = jack_transport_query(m_jack_client, &pos);

//     if (state != JackTransportRolling)
//         return 0;

//     if (!(pos.valid & JackPositionBBT))
//         return 0;

//     /*
//        First synchronization:
//        convert JACK BBT position into MIDI ticks
//     */

//     if (!initialized)
//     {
//         double jack_ticks =
//             (pos.bar - 1) * pos.beats_per_bar * pos.ticks_per_beat +
//             (pos.beat - 1) * pos.ticks_per_beat +
//             pos.tick;

//         midi_current_tick =
//             jack_ticks *
//             midi_tpq /
//             pos.ticks_per_beat;

//         // midi_current_tick = (int)midi_current_tick % (int)(pos.ticks_per_beat);
//         midi_current_tick = fmod(midi_current_tick, 768);
//         while (event_index < midifile[1].size() &&
//                midifile[1][event_index].tick < midi_current_tick)
//         {
//             event_index++;
//             if(event_index >= midifile[1].size()){
//                 event_index = 0;
//             }
//         }

//         printf("SYNC JACK bar= %d beat=%d tick=%d miditick=%d\n", pos.bar, pos.beat, pos.tick, midi_current_tick);

//         initialized = true;
//     }

//     jack_nframes_t jack_start =
//         pos.frame;

//     jack_nframes_t jack_end =
//         jack_start + nframes;

//     if (event_index >= midifile[1].size())
//         event_index = 0;

//     while (1)
//     {
//         if(event_index >= midifile[1].size()){
//             event_index = 0;
//         }
//         if(midifile[1][event_index].isNoteOn()){

//             double jack_tick =
//                 (pos.bar - 1) *
//                     pos.beats_per_bar *
//                     pos.ticks_per_beat
//                 +
//                 (pos.beat - 1) *
//                     pos.ticks_per_beat
//                 +
//                 pos.tick;

//             double midi_tick =
//                 jack_tick *
//                 midi_tpq /
//                 pos.ticks_per_beat;

//             midi_tick = fmod(midi_tick, 768);

//             int event_tick = midifile[1][event_index].tick;
//             // double tick_delta = fmod(abs(event_tick - midi_current_tick), 768);
//             double tick_delta = event_tick - midi_tick;

//             if (tick_delta < 0)
//                 tick_delta += 768;
//             /*
//             Convert MIDI ticks to JACK frames

//             tick_delta / MIDI TPQ = beats

//             beats * frames_per_beat
//             */

//             double frame_delta =
//                 (tick_delta / midi_tpq) *
//                 (pos.frame_rate *
//                 60.0 /
//                 pos.beats_per_minute);

//             jack_nframes_t event_frame =
//                 jack_start +
//                 (jack_nframes_t)round(frame_delta);

//             if (event_frame >= jack_end)
//                 break;

//             if (event_frame >= jack_start)
//             {
//                 jack_nframes_t offset =
//                     event_frame - jack_start;
//                     midifile[1][event_index].setChannel(event_index);
// printf(
//     "bar=%d beat=%d tick=%f jack_frame=%u midi_tick=%f event_tick=%d delta_tick=%f frame_delta=%f\n",
//     pos.bar,
//     pos.beat,
//     pos.tick,
//     pos.frame,
//     midi_current_tick,
//     event_tick,
//     tick_delta,
//     frame_delta);
//                     jack_midi_event_write(
//                         buffer,
//                         offset,
//                         midifile[1][event_index].data(),
//                         midifile[1][event_index].size());

//             }

//             midi_current_tick = event_tick;
//             event_index++;
//         }else{
//             event_index ++;
//         }
//     }

//     /*
//        Advance MIDI time according to JACK time

//        Convert frames -> MIDI ticks
//     */

//     double beats_elapsed =
//         nframes /
//         (pos.frame_rate *
//          60.0 /
//          pos.beats_per_minute);

//     midi_current_tick += beats_elapsed * midi_tpq;
//     return 0;
// }

void timebase_callback(jack_transport_state_t state, jack_nframes_t nframes,
                       jack_position_t *pos, int new_pos, void *arg)
{
    double min;    /* minutes since frame 0 */
    long abs_tick; /* ticks since frame 0 */
    long abs_beat; /* beats since frame 0 */

    if (new_pos || m_time_reset)
    {

        pos->valid = JackPositionBBT;
        pos->beats_per_bar = m_time_beats_per_bar;
        pos->beat_type = m_time_beat_type;
        pos->ticks_per_beat = m_time_ticks_per_beat;
        pos->beats_per_minute = m_time_beats_per_minute;

        m_time_reset = 0; /* time change complete */

        /* Compute BBT info from frame number.  This is relatively
         * simple here, but would become complex if we supported tempo
         * or time signature changes at specific locations in the
         * transport timeline.
         */

        min = pos->frame / ((double)pos->frame_rate * 60.0);
        abs_tick = min * pos->beats_per_minute * pos->ticks_per_beat;
        abs_beat = abs_tick / pos->ticks_per_beat;

        pos->bar = abs_beat / pos->beats_per_bar;
        pos->beat = abs_beat - (pos->bar * pos->beats_per_bar) + 1;
        m_last_tick = abs_tick - (abs_beat * pos->ticks_per_beat);
        pos->bar_start_tick = pos->bar * pos->beats_per_bar *
                              pos->ticks_per_beat;
        pos->bar++; /* adjust start to bar 1 */
    }
    else
    {
        /* Compute BBT info based on previous period. */
        m_last_tick +=
            nframes * pos->ticks_per_beat * pos->beats_per_minute / (pos->frame_rate * 60);

        while (m_last_tick >= pos->ticks_per_beat)
        {
            m_last_tick -= pos->ticks_per_beat;
            if (++pos->beat > pos->beats_per_bar)
            {
                pos->beat = 1;
                ++pos->bar;
                pos->bar_start_tick +=
                    pos->beats_per_bar * pos->ticks_per_beat;
            }
        }
    }

    pos->tick = (int)(m_last_tick + 0.5);
}

// void JackClient::set_midi_file(smf_t *midi_file)
// {
//     m_smf = midi_file;
//     if(m_midi_state != STOPPED)
//         m_midi_state = SYNC_REQUESTED;
// }

// void JackClient::set_state(bool state)
// {
//     if (m_smf == NULL)
//     {
//         std::cout << "No midi file loaded" << std::endl;
//     }

//     if (state)
//     {
//         m_midi_state = SYNC_REQUESTED;
//     }
//     else
//     {
//         m_midi_state = STOPPED;
//     }
// }

// void JackClient::set_bpm(int bpm)
// {
//     std::cout << "jack bpm : " << bpm << std::endl;
//     if (bpm > 20 && bpm < 150 && bpm != m_time_beats_per_minute)
//     {
//         m_time_beats_per_minute = bpm;
//         m_time_reset = 1;
//     }
// }

int connect_ports()
{
    const char **ports = jack_get_ports(m_jack_client, "pedal-synth", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);
    if (ports == NULL)
    {
        return 0;
    }

    if (jack_connect(m_jack_client, jack_port_name(m_midi_output_port), ports[0]))
    {
        std::cout << "cannot connect output ports" << std::endl;
        return -1;
    }
    jack_free(ports);

    ports = jack_get_ports(m_jack_client, "pedal-synth", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput);
    if (ports == NULL)
    {
        return 0;
    }

    if (jack_connect(m_jack_client, ports[0], "system:playback_1"))
    {
        std::cout << "cannot connect output ports" << std::endl;
        return -1;
    }

    if (jack_connect(m_jack_client, ports[1], "system:playback_2"))
    {
        std::cout << "cannot connect output ports" << std::endl;
        return -1;
    }
    jack_free(ports);

    return 0;
}

void port_registration_callback(jack_port_id_t port, int reg, void *arg)
{
    //     if (reg == 0)
    //     {
    //         return;
    //     }
    //     JackClient *p_client = (JackClient *)arg;

    //     jack_port_t *p = jack_port_by_id(p_client->m_jack_client, port);
    //     if (p == NULL)
    //     {
    //         return;
    //     }

    //     std::string port_name(jack_port_name(p));

    //     if (!jack_port_is_mine(p_client->m_jack_client, p) &&
    //         strcmp(jack_port_type(p), JACK_DEFAULT_MIDI_TYPE) == 0 &&
    //         (jack_port_flags(p) & JackPortIsInput))
    //     {
    //         if (port_name.find("qsynth") != std::string::npos || port_name.find("midi-monitor") != std::string::npos)
    //         {
    //             jack_connect(p_client->m_jack_client, jack_port_name(p_client->m_midi_output_port), jack_port_name(p));
    //         }
    //     }
}

int start(void)
{
    int i, err;
    m_jack_client = jack_client_open("sequencer", JackNullOption, NULL);

    if (m_jack_client == NULL)
    {
        std::cout << "Could not connect to the JACK server; run jackd first?" << std::endl;
        return -1;
    }

    err = jack_set_process_callback(m_jack_client, process_on_beat, NULL);
    if (err)
    {
        std::cout << "Could not register JACK process callback." << std::endl;
        return -1;
    }

    // err = jack_set_timebase_callback(m_jack_client, 0, timebase_callback, NULL);
    // if (err)
    // {
    //     std::cout << "Could not register JACK timebase callback %d." << std::endl;
    //     return -1;
    // }

    err = jack_set_port_registration_callback(m_jack_client, port_registration_callback, NULL);
    if (err)
    {
        std::cout << "Could not register JACK port register callback %d." << std::endl;
        return -1;
    }

    m_midi_output_port = jack_port_register(m_jack_client, "output", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
    if (m_midi_output_port == NULL)
    {
        std::cout << "Could not register JACK output port " << "output" << std::endl;
        return -1;
    }

    if (jack_activate(m_jack_client))
    {
        std::cout << "Cannot activate JACK client." << std::endl;
        return -1;
    }

    connect_ports();

    // jack_transport_locate(m_jack_client, 0);
    // jack_transport_start(m_jack_client);

    return 0;
}

void set_bpm(int bpm)
{
    cout << "jack bpm : " << bpm << endl;
    if (bpm > 20 && bpm < 150 && bpm != m_time_beats_per_minute)
    {
        m_time_beats_per_minute = bpm;
        m_time_reset = 1;
    }
}

void load_midi_file(std::string file_path)
{
    midifile.read(file_path);
    // midifile.joinTracks();
    midifile.doTimeAnalysis();
    midifile.linkNotePairs();

    int tracks = midifile.getTrackCount();

    cout << "**********************" << endl;
    cout << "MIDI FILE: " << file_path << endl;
    std::cout << "Ticks: " << midifile.getFileDurationInTicks() << '\n';
    cout << "MIDI DURATION : " << std::to_string(midifile.getFileDurationInSeconds()) << endl;
    cout << "TPQ: " << midifile.getTicksPerQuarterNote() << endl;
    if (tracks > 1)
        cout << "TRACKS: " << tracks << endl;
    for (int track = 0; track < tracks; track++)
    {
        if (tracks > 1)
            cout << "\nTrack " << track << endl;
        cout << "Index\tTick\tSeconds\tDur\tMessage" << endl;
        for (int event = 0; event < midifile[track].size(); event++)
        {
            if (midifile[track][event].isMeta() && midifile[track][event].getMetaType() == 0x03)
            {
                std::string trackName = midifile[track][event].getMetaContent();
                if (!trackName.empty())
                    cout << trackName << endl;
            }

            if (!midifile[track][event].isMetaMessage() && !midifile[track][event].isNoteOff())
            {
                cout << event;
                cout << '\t' << dec << midifile[track][event].tick;
                cout << '\t' << dec << midifile[track][event].seconds;
                cout << '\t';
                cout << midifile[track][event].getDurationInSeconds();
                cout << '\t' << hex;
                for (int i = 0; i < midifile[track][event].size(); i++)
                    cout << (int)midifile[track][event][i] << ' ';
                cout << endl;
            }
        }
    }
    cout << "**********************" << endl;
    // midifile.joinTracks();
    m_midi_state = SYNC_REQUESTED;
}

bool stop = false;
void signal_callback(int sig)
{
    stop = true;
}

int main(int argc, char **argv)
{
    start();
    load_midi_file("/home/marius/emptySong.mid");

    int i = 0;
    while (!stop)
    {
        sleep(1);
        i++;
        if (i == 10)
        {
            set_bpm(60);
        }
    }

    return 0;
}