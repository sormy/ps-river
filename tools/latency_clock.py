"""Wall clock in milliseconds, for measuring stream latency: full screen on the host, and with
--window on the client, next to the stream, so one screenshot shows both. A key press flips the
background, to time input round trips the same way."""

import argparse
import time
import tkinter as tk

FONT = ("DejaVu Sans Mono", 120, "bold")
REFRESH_MS = 1
COLOURS = ("black", "navy")


class LatencyClock:
    def __init__(self, root: tk.Tk, fullscreen: bool) -> None:
        self.root = root
        self.colour = 0
        self.label = tk.Label(root, font=FONT, fg="white", bg=COLOURS[0])
        self.label.pack(expand=True, fill="both")
        root.attributes("-fullscreen", fullscreen)
        root.attributes("-topmost", True)
        root.bind("<Key>", self.on_key)
        root.bind("<Escape>", lambda _: root.destroy())

    def on_key(self, _: tk.Event) -> None:
        self.colour = 1 - self.colour
        self.label.configure(bg=COLOURS[self.colour])

    def tick(self) -> None:
        now = time.time()
        self.label.configure(text=time.strftime("%H:%M:%S", time.localtime(now)) + f".{int(now * 1000) % 1000:03d}")
        self.root.after(REFRESH_MS, self.tick)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--window", action="store_true", help="a window instead of full screen")
    args = parser.parse_args()
    root = tk.Tk()
    LatencyClock(root, fullscreen=not args.window).tick()
    root.mainloop()


if __name__ == "__main__":
    main()
