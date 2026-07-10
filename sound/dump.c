#include <stdio.h>
#include <jack/jack.h>
#include <jack/midiport.h>

static jack_client_t *client;


static void print_bbt_for_event(jack_nframes_t event_frame)
{
    jack_position_t pos;

    jack_transport_query(client, &pos);

    if (!(pos.valid & JackPositionBBT))
        return;


    /*
     * Difference between the JACK transport position
     * and the MIDI event position
     */
    jack_nframes_t frame_diff =
        event_frame - pos.frame;


    double sample_rate =
        jack_get_sample_rate(client);


    /*
     * Convert frames to beats
     */
    double beats_offset =
        (frame_diff / sample_rate)
        * (pos.beats_per_minute / 60.0);


    double ticks_offset =
        beats_offset * pos.ticks_per_beat;


    double tick =
        pos.tick + ticks_offset;


    int beat = pos.beat;
    int bar  = pos.bar;


    /*
     * Normalize ticks overflow
     */
    while (tick >= pos.ticks_per_beat) {
        tick -= pos.ticks_per_beat;
        beat++;

        if (beat > pos.beats_per_bar) {
            beat = 1;
            bar++;
        }
    }


    printf("BBT: bar=%d beat=%d tick=%.2f\n",
           bar,
           beat,
           tick);
}


static int process(jack_nframes_t nframes, void *arg)
{
    jack_port_t *port = arg;

    void *buffer =
        jack_port_get_buffer(port, nframes);


    uint32_t count =
        jack_midi_get_event_count(buffer);


    for (uint32_t i = 0; i < count; i++) {

        jack_midi_event_t ev;

        jack_midi_event_get(
            &ev,
            buffer,
            i
        );


        if (ev.size < 3)
            continue;


        unsigned char status =
            ev.buffer[0] & 0xf0;


        unsigned char note =
            ev.buffer[1];


        unsigned char velocity =
            ev.buffer[2];


        if (status == 0x90 && velocity > 0) {

            jack_nframes_t frame =
                jack_last_frame_time(client)
                + ev.time;


            printf("\nNote %d velocity %d\n",
                   note,
                   velocity);


            printf("Frame: %u\n",
                   frame);


            print_bbt_for_event(frame);
        }
    }

    return 0;
}


int main()
{
    client =
        jack_client_open(
            "midi_bbt_monitor",
            JackNullOption,
            NULL);


    jack_port_t *in =
        jack_port_register(
            client,
            "midi_in",
            JACK_DEFAULT_MIDI_TYPE,
            JackPortIsInput,
            0);


    jack_set_process_callback(
        client,
        process,
        in);


    jack_activate(client);


    printf("Running\n");

    getchar();

    jack_client_close(client);

    return 0;
}