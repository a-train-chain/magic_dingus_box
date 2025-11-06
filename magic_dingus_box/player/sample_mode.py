from __future__ import annotations

from typing import Optional, List


class SampleModeManager:
    """Manages video sampling mode with timestamp markers.
    
    Allows users to set up to 4 markers at specific timestamps and jump between them
    for creative sampling or practice.
    """

    def __init__(self) -> None:
        self.active = False
        self.markers: List[Optional[float]] = [None, None, None, None]
        self.marker_order: List[int] = []  # Track set order for undo (stores slot indices)

    def toggle_active(self) -> None:
        """Toggle sample mode on/off."""
        self.active = not self.active

    def set_marker(self, slot: int, timestamp: float) -> None:
        """Set a marker at the given slot (0-3) with the provided timestamp.
        
        Args:
            slot: Marker slot index (0-3 for keys 1-4)
            timestamp: Video timestamp in seconds
        """
        if 0 <= slot < 4:
            # If this slot already has a marker, remove it from order list
            if self.markers[slot] is not None and slot in self.marker_order:
                self.marker_order.remove(slot)
            
            self.markers[slot] = timestamp
            self.marker_order.append(slot)

    def get_marker(self, slot: int) -> Optional[float]:
        """Get the timestamp for a given marker slot.
        
        Args:
            slot: Marker slot index (0-3 for keys 1-4)
            
        Returns:
            Timestamp in seconds, or None if not set
        """
        if 0 <= slot < 4:
            return self.markers[slot]
        return None

    def undo_last_marker(self) -> None:
        """Remove the most recently placed marker."""
        if self.marker_order:
            last_slot = self.marker_order.pop()
            self.markers[last_slot] = None

    def clear_markers(self) -> None:
        """Clear all markers and order history."""
        self.markers = [None, None, None, None]
        self.marker_order.clear()

    def is_marker_set(self, slot: int) -> bool:
        """Check if a marker is set at the given slot.
        
        Args:
            slot: Marker slot index (0-3 for keys 1-4)
            
        Returns:
            True if marker is set, False otherwise
        """
        if 0 <= slot < 4:
            return self.markers[slot] is not None
        return False

    def get_marker_states(self) -> List[bool]:
        """Get a list of marker states (True if set, False if not).
        
        Returns:
            List of 4 boolean values representing marker states
        """
        return [marker is not None for marker in self.markers]

