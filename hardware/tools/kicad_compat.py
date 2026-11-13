"""pcbnew compatibility for the hardware scripts: import this instead of
pcbnew (`from kicad_compat import pcbnew`).

KiCad 10's SWIG bindings (seen with 10.0.6 on Ubuntu 26.10) iterate deque-
backed containers through SwigPyIterator.next(), which that SWIG build no
longer defines (it has only __next__), so board.GetTracks(),
board.GetDrawings() and footprint.GraphicalItems() raise AttributeError.
Indexed containers (footprints, pads, zones) are unaffected. Aliasing next to
__next__ restores them; on KiCad 9 the alias is already there and this does
nothing.
"""
import pcbnew

_it = getattr(pcbnew, "SwigPyIterator", None)
if _it is not None and not hasattr(_it, "next") and hasattr(_it, "__next__"):
    _it.next = _it.__next__

KICAD_MAJOR = int(pcbnew.GetMajorMinorVersion().split(".")[0]) if hasattr(pcbnew, "GetMajorMinorVersion") else 0


def board_polygon_outlines(board, poly):
    """board.GetBoardPolygonOutlines(poly): KiCad 10 added a required
    aInferOutlineIfNecessary argument (False keeps 9's behaviour: use the
    Edge.Cuts outline as drawn)."""
    try:
        return board.GetBoardPolygonOutlines(poly, False)
    except TypeError:
        return board.GetBoardPolygonOutlines(poly)
