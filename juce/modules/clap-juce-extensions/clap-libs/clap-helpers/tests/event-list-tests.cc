#include <clap/helpers/event-list.hh>

#include <catch2/catch_all.hpp>

namespace {

   CATCH_TEST_CASE("EventList - ctor") { clap::helpers::EventList list; }

   CATCH_TEST_CASE("EventList - sysex") {
      clap::helpers::EventList list;

      {
         // craft a sysex event
         uint8_t sysex[5] = {0, 1, 2, 3, 4};
         clap_event_midi_sysex ev;
         ev.header.size = sizeof (ev);
         ev.header.time = 0;
         ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
         ev.header.type = CLAP_EVENT_MIDI_SYSEX;
         ev.header.flags = 0;
         ev.buffer = sysex;
         ev.size = sizeof (sysex);
         ev.port_index = 0;

         // add it to the list
         list.push(&ev.header);

         // Trash the sysex memory
         std::fill_n(sysex, sizeof (sysex), 0xff);
      }

      auto hdr = list.get(0);
      CATCH_CHECK(hdr);
      CATCH_CHECK(hdr->space_id == CLAP_CORE_EVENT_SPACE_ID);
      CATCH_CHECK(hdr->type == CLAP_EVENT_MIDI_SYSEX);

      auto ev = reinterpret_cast<const clap_event_midi_sysex *>(hdr);
      CATCH_CHECK(ev->size == 5);
      for (int i = 0; i < 5; ++i)
         CATCH_CHECK(ev->buffer[i] == i);
   }

} // namespace