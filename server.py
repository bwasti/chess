import socket
import sys
import threading
import signal
import sys
import chess
import chess.engine


def signal_handler(signal, frame):
    sys.exit(0)


signal.signal(signal.SIGINT, signal_handler)

HOST = "0.0.0.0"  # all availabe interfaces
PORT = 9999  # arbitrary non privileged port

try:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
except socket.error as msg:
    print("Could not create socket. Error Code: ", str(msg[0]), "Error: ", msg[1])
    sys.exit(0)

print("[-] Socket Created")

# bind socket
try:
    s.bind((HOST, PORT))
    print("[-] Socket Bound to port " + str(PORT))
except socket.error as msg:
    print("Bind Failed. Error Code: {} Error: {}".format(str(msg[0]), msg[1]))
    sys.exit()

s.listen(10)
print("Listening...")

# The code below is what you're looking for ############

conns = []
conns_lock = threading.Lock()

board = chess.Board()
# engine = chess.engine.SimpleEngine.popen_uci("/Applications/Stockfish.app/Contents/MacOS/Stockfish", timeout=60)
# print(engine.analyse(board, chess.engine.Limit(time=0.5))["score"])
def update_board(move):
    print(move)
    try:
        if len(move):
            board.push_uci(move.decode("utf-8").strip())
    except:
        print(f"illegal move {move}")
    print("\033c", end="")
    print(board)
    #print(board.unicode(empty_square=" ",invert_color=True))
    # print(engine.analyse(board, chess.engine.Limit(time=0.5))["score"])


def client_thread(conn):
    while True:
        data = conn.recv(1024)
        if not data:
            break
        update_board(data)
        with conns_lock:
            for c in [_ for _ in conns]:
                if c == conn:
                    continue
                try:
                    c.sendall(data)
                except:
                    conns.remove(c)
    conn.close()


while True:
    # blocking call, waits to accept a connection
    conn, addr = s.accept()
    print("[-] Connected to " + addr[0] + ":" + str(addr[1]))
    with conns_lock:
        conns.append(conn)
    t = threading.Thread(target=client_thread, args=(conn,))
    t.start()

s.close()
