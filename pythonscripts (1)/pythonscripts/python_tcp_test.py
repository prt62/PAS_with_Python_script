import socket
import os
import time

# --- Global Configuration ---
target_ip = '192.168.0.222' 
PORT = 5500
FILE_NAME = 'song.mp3' 

def send_command(cmd):
    """
    Connects, sends exactly 4 bytes, waits for ACK, and closes politely.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(2.0)
    
    try:
        sock.connect((target_ip, PORT))
        
        # 1. THE FIX: Send EXACTLY 4 bytes. No "\n" allowed!
        # The ESP32 is expecting exactly "PLAY" or "STOP"
        payload = cmd.strip() 
        sock.sendall(payload.encode())
        
        # 2. Wait for the ACK from ESP32
        ack = sock.recv(1024).decode().strip()
        
        if "ACK" in ack:
            return True
        else:
            print(f"[-] Received unexpected response: {ack}")
            return False
            
    except socket.timeout:
        print("[-] Command timed out. ESP32 is busy.")
        return False
    except Exception as e:
        print(f"[-] Network Error: {e}")
        return False
    finally:
        # 3. THE FIX: Restore the polite TCP FIN Handshake!
        # This tells the ESP32's strict LwIP stack that we are done talking gracefully.
        try:
             sock.shutdown(socket.SHUT_RDWR) 
        except Exception:
             pass 
         
        sock.close()

def upload_file():
    if not os.path.exists(FILE_NAME):
        print(f"[-] ERROR: '{FILE_NAME}' not found.")
        return

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5.0)
    
    try:
        sock.connect((target_ip, PORT))
        
        # THE FIX: Send strictly 4 bytes. 
        # If we send UPLD\n, the \n gets written into the MP3 file!
        sock.sendall("UPLD".encode())
        time.sleep(0.1) # Give ESP32 a tiny moment to switch to SD write mode
        
        print(f"[*] Uploading {FILE_NAME} to {target_ip}...")
        file_size = os.path.getsize(FILE_NAME)
        bytes_sent = 0

        with open(FILE_NAME, 'rb') as f:
            while True:
                chunk = f.read(4096) # Match ESP32's fast 4KB buffer
                if not chunk: break
                sock.sendall(chunk)
                time.sleep(0.002) # Fast transfer delay
                
                bytes_sent += len(chunk)
                percent = (bytes_sent / file_size) * 100
                print(f"[*] Progress: {bytes_sent}/{file_size} bytes ({percent:.1f}%) sent...", end='\r')
                
        print("\n[+] Transfer complete! Waiting for ESP32 confirmation...")
            
    except Exception as e:
        print(f"\n[-] Upload Error: {e}")
    finally:
        try:
             sock.shutdown(socket.SHUT_RDWR) 
        except Exception:
             pass
        sock.close()

def main_menu():
    global target_ip
    
    while True:
        print(f"\n=== ESP32 AUDIO CONTROLLER ===")
        print(f"CURRENT TARGET IP: [{target_ip}]")
        print("-" * 30)
        print("0. Change Target IP Address")
        print("1. Upload new song")
        print("2. Play saved song")
        print("3. Stop audio")
        print("4. Exit")
        
        choice = input("Select command (0-4): ")
        
        if choice == '0':
            new_ip = input("Enter new ESP32 IP address: ")
            if new_ip.strip(): 
                target_ip = new_ip.strip()
                print(f"[+] Target IP updated to {target_ip}")
                
        elif choice == '1':
            upload_file()
            
        elif choice == '2':
            print("[*] Sending PLAY...")
            success = False
            attempts = 0
            while not success:
                attempts += 1
                if send_command('PLAY'):
                    print(f"[+] Play command confirmed after {attempts} attempt(s)!")
                    success = True
                else:
                    print(f"[-] PLAY failed. Retrying (Attempt {attempts + 1})...")
                    time.sleep(1.5)
                                
        elif choice == '3':
            print("[*] Sending STOP...")
            success = False
            attempts = 0
            while not success:
                attempts += 1
                if send_command('STOP'):
                    print(f"[+] Stop command confirmed after {attempts} attempt(s)!")
                    success = True
                else:
                    print(f"[-] STOP failed. ESP32 might be busy. Retrying (Attempt {attempts + 1})...")
                    time.sleep(1.5)
                
        elif choice == '4':
            print("Exiting...")
            break
        else:
            print("Invalid choice.")

if __name__ == "__main__":
    main_menu()