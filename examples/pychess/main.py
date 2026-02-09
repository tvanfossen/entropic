import tkinter as tk


class Chessboard:
    def __init__(self, root):
        self.root = root
        self.root.title("Chessboard")
        self.board_frame = tk.Frame(self.root)
        self.board_frame.pack(padx=10, pady=10)

        # Create 8x8 grid of squares
        for row in range(8):
            for col in range(8):
                color = "#f0d9b5" if (row + col) % 2 == 0 else "#b58863"
                cell = tk.Label(
                    self.board_frame,
                    bg=color,
                    width=4,
                    height=2,
                    relief="raised",
                    borderwidth=1
                )
                cell.grid(row=row, column=col, padx=1, pady=1)

if __name__ == "__main__":
    root = tk.Tk()
    Chessboard(root)
    root.mainloop()
