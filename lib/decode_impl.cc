/* -*- c++ -*- */
/* 
 * Copyright 2016 <+YOU OR YOUR COMPANY+>.
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include "decode_impl.h"

#define HAMMING_T1_BITMASK 0xAA  // 0b10101010
#define HAMMING_T2_BITMASK 0x66  // 0b01100110
#define HAMMING_T4_BITMASK 0x1E  // 0b00011110
#define HAMMING_T8_BITMASK 0xFE  // 0b11111110

#define INTERLEAVER_BLOCK_SIZE 8

namespace gr {
  namespace lora {

    decode::sptr
    decode::make(   short spreading_factor,
                    short code_rate)
    {
      return gnuradio::get_initial_sptr
        (new decode_impl(8, 4));
    }

    /*
     * The private constructor
     */
    decode_impl::decode_impl( short spreading_factor,
                              short code_rate)
      : gr::block("decode",
              gr::io_signature::make(0, 0, 0),
              gr::io_signature::make(0, 0, 0))
    {
      d_in_port = pmt::mp("in");
      d_out_port = pmt::mp("out");

      message_port_register_in(d_in_port);
      message_port_register_out(d_out_port);

      set_msg_handler(d_in_port, boost::bind(&decode_impl::decode, this, _1));

      d_sf = 8;
      d_cr = 4; 

      d_interleaver_size = d_sf;

      d_fft_size = (1 << spreading_factor);
    }

    /*
     * Our virtual destructor.
     */
    decode_impl::~decode_impl()
    {
    }

    void
    decode_impl::to_gray(std::vector<unsigned short> &symbols)
    {
      for (int i = 0; i < symbols.size(); i++)
      {
        symbols[i] = (symbols[i] >> 1) ^ symbols[i];
      }
    }

    void
    decode_impl::from_gray(std::vector<unsigned short> &symbols)
    {
      for (int i = 0; i < symbols.size(); i++)
      {
        symbols[i] = symbols[i] ^ (symbols[i] >> 16);
        symbols[i] = symbols[i] ^ (symbols[i] >>  8);
        symbols[i] = symbols[i] ^ (symbols[i] >>  4);
        symbols[i] = symbols[i] ^ (symbols[i] >>  2);
        symbols[i] = symbols[i] ^ (symbols[i] >>  1);
      }
    }

    void
    decode_impl::whiten(std::vector<unsigned short> &symbols)
    {
      for (int i = 0; i < symbols.size(); i++)
      {
        symbols[i] = ((unsigned char)(symbols[i] & 0xFF) ^ d_whitening_sequence[i]) & 0xFF;
      }
    }

    void
    decode_impl::deinterleave(std::vector <unsigned short> &symbols,
                              std::vector <unsigned char> &codewords)
    {
      unsigned char ppm = 8;
      int inner = 0;
      int outer = 0;
      unsigned char block[INTERLEAVER_BLOCK_SIZE] = {0};
      unsigned char word = 0;

      for (outer = 0; outer < symbols.size()/ppm; outer++)
      {
        memset(block, 0, d_interleaver_size*sizeof(unsigned char));

        for (inner = 0; inner < ppm; inner++)
        {
          // Most significant bits are flipped
          word = ((128 & symbols[ppm*outer+(inner+2)%ppm]) >> 1) | ((64 & symbols[ppm*outer+(inner+1)%ppm]) << 1) | (32 & symbols[ppm*outer+(inner+3)%ppm]) | (16 & symbols[ppm*outer+(inner+4)%ppm]) \
                    | (8 & symbols[ppm*outer+(inner+5)%ppm]) | (4 & symbols[ppm*outer+(inner+6)%ppm]) | (2 & symbols[ppm*outer+(inner+7)%ppm]) | (1 & symbols[ppm*outer+(inner+8)%ppm]);

          // Reverse endianness
          word = ((word & 128) >> 7) | ((word & 64) >> 5) | ((word & 32) >> 3) | ((word & 16) >> 1) | ((word & 8) << 1) | ((word & 4) << 3) | ((word & 2) << 5) | ((word & 1) << 7);
          word &= 0xFF;

          // Rotate
          word = (word << (inner+1)%ppm) | (word >> (ppm-inner-1)%ppm);
          word &= 0xFF;

          // Reorder into hamming format. Hamming parity bits p3 and p4 are flipped OTA, so format to be corrected is p1,p2,p4,p3,d1,d2,d3,d4
          // Final bit order is p1,p2,d1,p3,d2,d3,d4,p4
          word = ((word & 128)) | ((word & 64)) | ((word & 32) >> 5) | ((word & 16)) | ((word & 8) << 2) | ((word & 4) << 1) | ((word & 2) << 1) | ((word & 1) << 1);

          block[inner] = word & 0xFF;
        }

        codewords.push_back(block[0]);
        codewords.push_back(block[7]);
        codewords.push_back(block[2]);
        codewords.push_back(block[1]);
        codewords.push_back(block[4]);
        codewords.push_back(block[3]);
        codewords.push_back(block[6]);
        codewords.push_back(block[5]);
      }
    }

    void
    decode_impl::hamming_decode(std::vector<unsigned char> &codewords,
                                std::vector<unsigned char> &bytes)
    {
      unsigned char t1, t2, t4, t8;
      unsigned char mask;
      char error_pos = 0;

      for (int i = 0; i < codewords.size(); i++)
      {
        t1 = parity((unsigned char)codewords[i], mask = (unsigned char)HAMMING_T1_BITMASK);
        t2 = parity((unsigned char)codewords[i], mask = (unsigned char)HAMMING_T2_BITMASK);
        t4 = parity((unsigned char)codewords[i], mask = (unsigned char)HAMMING_T4_BITMASK);
        t8 = parity((unsigned char)codewords[i], mask = (unsigned char)HAMMING_T8_BITMASK);

        error_pos = -1;
        if (t1 != 0) error_pos += 1;
        if (t2 != 0) error_pos += 2;
        if (t4 != 0) error_pos += 4;

        if (error_pos >= 0)
        {
          codewords[i] ^= 0x80 >> error_pos;
        }

        codewords[i] = ((codewords[i] & 0x20) >> 2 | \
                        (codewords[i] & 0x08) >> 1 | \
                        (codewords[i] & 0x04) >> 1 | \
                        (codewords[i] & 0x02) >> 1) & 0x0F;

        if (i%2 == 1)
        {
          bytes.push_back(((codewords[i-1] & 0x0F) << 4) | (codewords[i] & 0x0F));
        }
      }
    }

    unsigned char
    decode_impl::parity(unsigned char c, unsigned char bitmask)
    {
      unsigned char parity = 0;
      unsigned char shiftme = c & bitmask;

      for (int i = 0; i < 8; i++)
      {
        if (shiftme & 0x1) parity++;
        shiftme = shiftme >> 1;
      }

      return parity % 2;
    }

    void
    decode_impl::print_payload(std::vector<unsigned char> &payload)
    {
        std::cout << "Received LoRa packet (hex): ";
        for (int i = 0; i < payload.size(); i++)
        {
          std::cout << std::hex << (unsigned int)payload[i] << " ";
        }
        std::cout << std::endl;
    }

    void
    decode_impl::decode(pmt::pmt_t msg)
    {
      pmt::pmt_t symbols(pmt::cdr(msg));

      size_t pkt_len(0);
      const uint16_t* symbols_v = pmt::u16vector_elements(symbols, pkt_len);

      std::vector<unsigned short> symbols_in;
      std::vector<unsigned char> codewords;
      std::vector<unsigned char> bytes;

      for (int i = 0; i < pkt_len; i++) symbols_in.push_back(symbols_v[i]);

      // Lop off preamble, sync word, and SFD -- now properly framed by demod
      // symbols_in.erase(symbols_in.begin(), symbols_in.begin()+8);

      to_gray(symbols_in);
      // std::cout << "DECODE 0" << std::endl;
      whiten(symbols_in);
      // std::cout << "DECODE 1" << std::endl;

      // Remove header until whitening sequence is extended
      // std::cout << "DECODE d_symbols size before: " << symbols_in.size() << std::endl;
      // d_symbols.erase(symbols_in.begin(), symbols_in.begin()+8);
      symbols_in.erase(symbols_in.begin(), symbols_in.begin()+8);
      // std::cout << "DECODE d_symbols size after: " << symbols_in.size() << std::endl;
      // std::cout << "DECODE 2" << std::endl;

      deinterleave(symbols_in, codewords);
      // std::cout << "DECODE 3" << std::endl;
      hamming_decode(codewords, bytes);
      // std::cout << "DECODE 4" << std::endl;

      print_payload(bytes);
      // std::cout << "DECODE 5" << std::endl;

      pmt::pmt_t output = pmt::init_u8vector(bytes.size(), bytes);
      pmt::pmt_t msg_pair = pmt::cons(pmt::make_dict(), output);
      message_port_pub(d_out_port, msg_pair);
    }

    void
    decode_impl::forecast (int noutput_items, gr_vector_int &ninput_items_required)
    {
       // <+forecast+> e.g. ninput_items_required[0] = noutput_items
       ninput_items_required[0] = noutput_items; 
    }

    int
    decode_impl::general_work (int noutput_items,
                       gr_vector_int &ninput_items,
                       gr_vector_const_void_star &input_items,
                       gr_vector_void_star &output_items)
    {
#if 0
      const unsigned short *in = (const unsigned short *) input_items[0];
      // unsigned char *out = (unsigned char *) output_items[0];
      unsigned short symbol = (unsigned short)*in;

      // std::cout << "LORA DECODER SYMBOL " << symbol << std::endl;

      // Do <+signal processing+>
      // Tell runtime system how many input items we consumed on
      // each input stream.

      d_symbols.push_back(symbol);

      if (symbol == d_fft_size)        // MAGIC word
      {
        to_gray(d_symbols);
        whiten(d_symbols);

        // Remove header until whitening sequence is extended
        d_symbols.erase(d_symbols.begin(), d_symbols.begin()+8);

        deinterleave(d_symbols, d_codewords);
        hamming_decode(d_codewords, d_bytes);

        print_payload(d_bytes);

        d_symbols.clear();
        d_codewords.clear();
        d_bytes.clear();
      }
      else if (d_symbols.size() > SYMBOL_TIMEOUT_COUNT)
      {
        d_symbols.clear();
      }

      consume_each (noutput_items);
#endif
      // Tell runtime system how many output items we produced.
      return noutput_items;
    }

  } /* namespace lora */
} /* namespace gr */

