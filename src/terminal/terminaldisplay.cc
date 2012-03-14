/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <boost/typeof/typeof.hpp>
#include <stdio.h>

#include "terminaldisplay.h"

using namespace Terminal;

/* Print a new "frame" to the terminal, using ANSI/ECMA-48 escape codes. */

static const Renditions & initial_rendition( void )
{
  const static Renditions blank = Renditions( 0 );
  return blank;
}

std::string Display::new_frame( bool initialized, const Framebuffer &last, const Framebuffer &f ) const
{
  FrameState frame( last );

  char tmp[ 64 ];

  /* has bell been rung? */
  if ( f.get_bell_count() != frame.last_frame.get_bell_count() ) {
    frame.os << '\x07';
  }

  /* has window title changed? */
  if ( (!initialized)
       || (f.get_window_title() != frame.last_frame.get_window_title()) ) {
      /* set window title */
    frame.os << "\033]0;";
    const std::deque<wchar_t> &window_title( f.get_window_title() );
    for ( BOOST_AUTO( i, window_title.begin() );
	  i != window_title.end();
	  i++ ) {
      snprintf( tmp, 64, "%lc", *i );
      frame.os << tmp;
    }
    frame.os << "\033\\";
  }

  /* has reverse video state changed? */
  if ( (!initialized)
       || (f.ds.reverse_video != frame.last_frame.ds.reverse_video) ) {
    /* set reverse video */
    frame.os << "\033[?5" << (f.ds.reverse_video ? 'h' : 'l');
  }

  /* has size changed? */
  if ( (!initialized)
       || (f.ds.get_width() != frame.last_frame.ds.get_width())
       || (f.ds.get_height() != frame.last_frame.ds.get_height()) ) {
    /* clear screen */
    frame.os << "\033[0m\033[H\033[2J";
    initialized = false;
    frame.cursor_x = frame.cursor_y = 0;
    frame.current_rendition = initial_rendition();
  } else {
    frame.cursor_x = frame.last_frame.ds.get_cursor_col();
    frame.cursor_y = frame.last_frame.ds.get_cursor_row();
    frame.current_rendition = frame.last_frame.ds.get_renditions();
  }

  /* shortcut -- has display moved up by a certain number of lines? */
  frame.y = 0;

  if ( initialized ) {
    int lines_scrolled = 0;
    int scroll_height = 0;

    for ( int row = 0; row < f.ds.get_height(); row++ ) {
      if ( *(f.get_row( 0 )) == *(frame.last_frame.get_row( row )) ) {
	/* found a scroll */
	lines_scrolled = row;
	scroll_height = 1;

	/* how big is the region that was scrolled? */
	for ( int region_height = 1;
	      lines_scrolled + region_height < f.ds.get_height();
	      region_height++ ) {
	  if ( *(f.get_row( region_height ))
	       == *(frame.last_frame.get_row( lines_scrolled + region_height )) ) {
	    scroll_height = region_height + 1;
	  } else {
	    break;
	  }
	}

	break;
      }
    }

    if ( scroll_height ) {
      frame.y = scroll_height;

      if ( lines_scrolled ) {
	if ( frame.cursor_y != f.ds.get_height() - 1 ) {
	  frame.append_silent_move( f.ds.get_height() - 1, 0 );
	}

	if ( !(frame.current_rendition == initial_rendition()) ) {
	  frame.os << "\033[0m";
	  frame.current_rendition = initial_rendition();
	}

	for ( int i = 0; i < lines_scrolled; i++ ) {
	  frame.os << '\n';
	}

	for ( int i = 0; i < f.ds.get_height(); i++ ) {
	  if ( i + lines_scrolled < f.ds.get_height() ) {
	    *(frame.last_frame.get_mutable_row( i )) = *(frame.last_frame.get_row( i + lines_scrolled ));
	  } else {
	    frame.last_frame.get_mutable_row( i )->reset( 0 );
	  }
	}
      }
    }
  }

  /* iterate for every cell */
  for ( ; frame.y < f.ds.get_height(); frame.y++ ) {
    int last_x = 0;
    for ( frame.x = 0;
	  frame.x < f.ds.get_width(); /* let put_cell() handle advance */ ) {
      last_x = frame.x;
      put_cell( initialized, frame, f );

      /* To hint that a word-select should group the end of one line
	 with the beginning of the next, we let the real cursor
	 actually wrap around in cases where it wrapped around for us. */

      if ( (frame.cursor_x >= f.ds.get_width())
	   && (frame.y < f.ds.get_height() - 1)
	   && f.get_row( frame.y )->wrap
	   && (!initialized || !frame.last_frame.get_row( frame.y )->wrap) ) {
	/* next write will wrap */
	frame.cursor_x = 0;
	frame.cursor_y++;
      }
    }

    /* Turn off wrap */
    if ( (frame.y < f.ds.get_height() - 1)
	 && (!f.get_row( frame.y )->wrap)
	 && (!initialized || frame.last_frame.get_row( frame.y )->wrap) ) {
      frame.x = last_x;
      if ( initialized ) {
	frame.last_frame.reset_cell( frame.last_frame.get_mutable_cell( frame.y, frame.x ) );
      }

      frame.os << "\033[" << frame.y + 1 << ';' << frame.x + 1 << "H\033[K";
      frame.cursor_x = frame.x;

      put_cell( initialized, frame, f );
    }
  }

  /* has cursor location changed? */
  if ( (!initialized)
       || (f.ds.get_cursor_row() != frame.cursor_y)
       || (f.ds.get_cursor_col() != frame.cursor_x) ) {
    frame.os << "\033[" << f.ds.get_cursor_row() + 1 << ';' << f.ds.get_cursor_col() + 1 << 'H';
    frame.cursor_x = f.ds.get_cursor_col();
    frame.cursor_y = f.ds.get_cursor_row();
  }

  /* has cursor visibility changed? */
  if ( (!initialized)
       || (f.ds.cursor_visible != frame.last_frame.ds.cursor_visible) ) {
    if ( f.ds.cursor_visible ) {
      frame.os << "\033[?25h";
    } else {
      frame.os << "\033[?25l";
    }
  }

  /* have renditions changed? */
  if ( (!initialized)
       || !(f.ds.get_renditions() == frame.current_rendition) ) {
    frame.os << f.ds.get_renditions().sgr();
    frame.current_rendition = f.ds.get_renditions();
  }

  return frame.os.str();
}

void Display::put_cell( bool initialized, FrameState &frame, const Framebuffer &f ) const
{
  char tmp[ 64 ];

  const Cell *cell = f.get_cell( frame.y, frame.x );

  if ( initialized
       && ( *cell == *(frame.last_frame.get_cell( frame.y, frame.x )) ) ) {
    frame.x += cell->width;
    return;
  }

  if ( (frame.x != frame.cursor_x) || (frame.y != frame.cursor_y) ) {
    frame.append_silent_move( frame.y, frame.x );
  }

  if ( !(frame.current_rendition == cell->renditions) ) {
    /* print renditions */
    frame.os << cell->renditions.sgr();
    frame.current_rendition = cell->renditions;
  }

  if ( cell->contents.empty() ) {
    /* see how far we can stretch a clear */
    int clear_count = 0;
    for ( int col = frame.x; col < f.ds.get_width(); col++ ) {
      const Cell *other_cell = f.get_cell( frame.y, col );
      if ( (cell->renditions == other_cell->renditions)
	   && (other_cell->contents.empty()) ) {
	clear_count++;
      } else {
	break;
      }
    }

    assert( frame.x + clear_count <= f.ds.get_width() );

    /* can we go to the end of the line? */
    if ( frame.x + clear_count == f.ds.get_width() ) {
      frame.os << "\033[K";
      frame.x += clear_count;
    } else {
      if ( has_ech ) {
	if ( clear_count == 1 ) {
	  frame.os << "\033[X";
	} else {
	  frame.os << "\033[" << clear_count << 'X';
	}
	frame.x += clear_count;
      } else { /* no ECH, so just print a space */
	frame.os << ' ';
	frame.cursor_x++;
	frame.x++;
      }
    }

    return;
  }

  /* cells that begin with combining character get combiner attached to no-break space */
  if ( cell->fallback ) {
    frame.os << "\xC2\xA0";
  }

  for ( std::vector<wchar_t>::const_iterator i = cell->contents.begin();
	i != cell->contents.end();
	i++ ) {
    snprintf( tmp, 64, "%lc", *i );
    frame.os << tmp;
  }

  frame.x += cell->width;
  frame.cursor_x += cell->width;
}

void FrameState::append_silent_move( int y, int x )
{
  /* turn off cursor if necessary before moving cursor */
  if ( last_frame.ds.cursor_visible ) {
    os << "\033[?25l";
    last_frame.ds.cursor_visible = false;
  }

  os << "\033[" << y + 1 << ';' << x + 1 << 'H';
  cursor_x = x;
  cursor_y = y;
}
