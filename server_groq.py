import os
import io
import wave
import base64
import subprocess
import tempfile
from flask import Flask, request, Response, jsonify
from groq import Groq
from gtts import gTTS

# ================= CONFIG =================
os.environ["GROQ_API_KEY"] = "YOUR_API_KEY_HERE"

SAMPLE_RATE  = 16000
CHANNELS     = 1
WIDTH        = 2
FRONT_OBSTACLE_THRESHOLD = 20   # cm
BACK_OBSTACLE_THRESHOLD  = 20   # cm
MAX_MEMORY   = 5                # conversation exchanges to remember

# ================= INIT =================
app    = Flask(__name__)
client = Groq()

# ================= STATE =================
latest_audio      = None
audio_ready       = False
front_distance    = 999   # cm (default = clear)
back_distance     = 999   # cm (default = clear)
conversation_memory = []  # list of {"role": ..., "content": ...}

# ESP32_A address (set this to your ESP32_A IP)
ESP32_A_IP = "10.84.51.208"  # Will update to actual ESP32_A IP later
ESP32_A_PORT = 80

# ================= AUDIO HELPERS =================
def pcm_to_wav(pcm):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(WIDTH)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)
    return buf.getvalue()

def text_to_pcm(text):
    tts = gTTS(text=text, lang="en")
    with tempfile.NamedTemporaryFile(delete=False, suffix=".mp3") as f:
        tts.save(f.name)
        mp3_path = f.name
    ffmpeg = subprocess.Popen(
        ["ffmpeg", "-y", "-i", mp3_path,
         "-filter:a", "volume=3.0,highpass=f=200,lowpass=f=3000",
         "-f", "s16le", "-ac", "1", "-ar", "16000", "pipe:1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    pcm_audio, _ = ffmpeg.communicate()
    os.remove(mp3_path)
    return pcm_audio

def update_memory(role, content):
    global conversation_memory
    conversation_memory.append({"role": role, "content": content})
    # Keep only last MAX_MEMORY exchanges (each exchange = 2 messages)
    if len(conversation_memory) > MAX_MEMORY * 2:
        conversation_memory = conversation_memory[-(MAX_MEMORY * 2):]

# ================= SYSTEM PROMPT =================
def build_system_prompt():
    return f"""You are ARIA (Autonomous Responsive Intelligent Assistant), a friendly and intelligent voice-controlled robot assistant.

CURRENT SENSOR DATA:
- Front obstacle distance: {front_distance} cm
- Back obstacle distance: {back_distance} cm

SAFETY RULES (STRICTLY FOLLOW):
- If front_distance < {FRONT_OBSTACLE_THRESHOLD} cm, NEVER allow forward movement. Warn the user.
- If back_distance < {BACK_OBSTACLE_THRESHOLD} cm, NEVER allow reverse movement. Warn the user.
- If both front and back are blocked, suggest turning left or right.
- If a command is inappropriate, harmful or dangerous, politely refuse and explain why.
- Always prioritize safety over user commands.

PERSONALITY:
- You are friendly, polite, and helpful.
- Keep responses concise (1-3 sentences) since they will be spoken aloud.
- Always explain your reasoning when refusing a command.
- Address the user warmly.

AVAILABLE TOOLS:
- answer_question: Use for general knowledge questions, conversations, explanations.
- control_robot: Use for movement commands (forward, backward, left, right, stop).
- refuse_command: Use for unsafe, inappropriate or impossible commands.
- describe_scene: Use when user asks what you see, what's in front, visual questions."""

# ================= TOOL DEFINITIONS =================
tools = [
    {
        "type": "function",
        "function": {
            "name": "answer_question",
            "description": "Answer a general knowledge question or have a conversation with the user.",
            "parameters": {
                "type": "object",
                "properties": {
                    "answer": {
                        "type": "string",
                        "description": "The spoken answer to give the user. Keep it concise."
                    }
                },
                "required": ["answer"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "control_robot",
            "description": "Move the robot. Only call this after verifying sensor data allows safe movement.",
            "parameters": {
                "type": "object",
                "properties": {
                    "direction": {
                        "type": "string",
                        "enum": ["forward", "backward", "left", "right", "stop"],
                        "description": "Direction to move the robot."
                    },
                    "speed": {
                        "type": "string",
                        "enum": ["slow", "normal", "fast"],
                        "description": "Speed of movement."
                    },
                    "spoken_response": {
                        "type": "string",
                        "description": "What ARIA says while executing the command."
                    }
                },
                "required": ["direction", "speed", "spoken_response"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "refuse_command",
            "description": "Refuse an unsafe, inappropriate or impossible command. Always explain why.",
            "parameters": {
                "type": "object",
                "properties": {
                    "reason": {
                        "type": "string",
                        "description": "Spoken explanation of why the command is being refused."
                    }
                },
                "required": ["reason"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "describe_scene",
            "description": "User is asking about what the camera sees. Trigger camera capture and vision analysis.",
            "parameters": {
                "type": "object",
                "properties": {
                    "prompt": {
                        "type": "string",
                        "description": "What specifically to look for or describe in the camera image."
                    }
                },
                "required": ["prompt"]
            }
        }
    }
]

# ================= VISION =================
def analyze_image(image_bytes, prompt):
    """Send image to Llama 4 Scout vision model via Groq."""
    try:
        image_b64 = base64.b64encode(image_bytes).decode("utf-8")
        response = client.chat.completions.create(
            model="meta-llama/llama-4-scout-17b-16e-instruct",
            messages=[
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "image_url",
                            "image_url": {
                                "url": f"data:image/jpeg;base64,{image_b64}"
                            }
                        },
                        {
                            "type": "text",
                            "text": f"You are ARIA, a robot assistant. {prompt}. Keep your answer to 1-2 sentences."
                        }
                    ]
                }
            ],
            max_tokens=150
        )
        return response.choices[0].message.content.strip()
    except Exception as e:
        print("❌ Vision error:", e)
        return "I'm having trouble seeing right now."

def capture_camera_frame():
    """Request a camera frame from ESP32-CAM."""
    import requests
    try:
        resp = requests.get(f"http://{ESP32_A_IP}/capture", timeout=5)
        if resp.status_code == 200:
            return resp.content
    except Exception as e:
        print("❌ Camera capture error:", e)
    return None

# ================= MOTOR CONTROL =================
def send_motor_command(direction, speed):
    """Send motor command to ESP32_A which relays to Arduino."""
    import requests
    try:
        requests.post(
            f"http://{ESP32_A_IP}/move",
            json={"direction": direction, "speed": speed},
            timeout=3
        )
        print(f"🤖 Motor command sent: {direction} at {speed}")
    except Exception as e:
        print("❌ Motor command error:", e)

# ================= WAKE WORD CHECK =================
def contains_wake_word(text, wake_word):
    return wake_word.lower() in text.lower()

# ================= AGENT =================
def run_agent(user_text, wake_word):
    global latest_audio, audio_ready

    # Wake word check
    if not contains_wake_word(user_text, wake_word):
        print(f"⏭️ No wake word detected in: '{user_text}'")
        return

    # Strip wake word from text before sending to LLM
    clean_text = user_text.lower().replace(wake_word.lower(), "").strip()
    if not clean_text:
        clean_text = "hello"

    print(f"🧠 Agent processing: '{clean_text}'")
    update_memory("user", clean_text)

    # Build messages with memory
    messages = [{"role": "system", "content": build_system_prompt()}]
    messages += conversation_memory[:-1]  # history excluding current
    messages.append({"role": "user", "content": clean_text})

    # First LLM call — agent decides which tool to use
    try:
        response = client.chat.completions.create(
            model="llama-3.3-70b-versatile",
            messages=messages,
            tools=tools,
            tool_choice="auto",
            max_tokens=300
        )
    except Exception as e:
        print("❌ LLM error:", e)
        return

    msg = response.choices[0].message

    # No tool call — direct text response
    if not msg.tool_calls:
        spoken = msg.content.strip() if msg.content else "I'm not sure how to respond to that."
        update_memory("assistant", spoken)
        _speak(spoken)
        return

    # Process tool call
    tool_call = msg.tool_calls[0]
    tool_name  = tool_call.function.name
    import json
    tool_args  = json.loads(tool_call.function.arguments)

    print(f"🔧 Tool called: {tool_name} with args: {tool_args}")

    if tool_name == "answer_question":
        spoken = tool_args.get("answer", "I don't know.")
        update_memory("assistant", spoken)
        _speak(spoken)

    elif tool_name == "control_robot":
        direction = tool_args.get("direction", "stop")
        speed     = tool_args.get("speed", "normal")
        spoken    = tool_args.get("spoken_response", f"Moving {direction}.")

        # Safety confidence check before executing
        safe = True
        if direction == "forward" and front_distance < FRONT_OBSTACLE_THRESHOLD:
            spoken = f"I can't move forward. There's an obstacle only {front_distance} centimeters ahead."
            safe = False
        elif direction == "backward" and back_distance < BACK_OBSTACLE_THRESHOLD:
            spoken = f"I can't reverse. There's an obstacle {back_distance} centimeters behind me."
            safe = False

        if safe:
            send_motor_command(direction, speed)

        update_memory("assistant", spoken)
        _speak(spoken)

    elif tool_name == "refuse_command":
        spoken = tool_args.get("reason", "I can't do that.")
        update_memory("assistant", spoken)
        _speak(spoken)

    elif tool_name == "describe_scene":
        prompt = tool_args.get("prompt", "Describe what you see.")
        image_bytes = capture_camera_frame()
        if image_bytes:
            spoken = analyze_image(image_bytes, prompt)
        else:
            spoken = "I'm unable to access my camera right now."
        update_memory("assistant", spoken)
        _speak(spoken)

def _speak(text):
    """Convert text to PCM audio and set ready flag."""
    global latest_audio, audio_ready
    print(f"🔊 ARIA says: {text}")
    pcm = text_to_pcm(text)
    if pcm and len(pcm) > 1000:
        latest_audio = pcm
        audio_ready  = True
        print(f"✅ Audio ready: {len(pcm)} bytes")
    else:
        print("❌ TTS failed")

# ================= ROUTES =================
@app.route("/process-audio", methods=["POST"])
def process_audio():
    global audio_ready
    audio_ready = False

    # Get wake word from header (ESP32 sends it)
    wake_word = request.headers.get("X-Wake-Word", "ARIA")

    print("🎤 Audio received")
    raw_pcm = request.data
    if not raw_pcm:
        return "No audio", 400

    # STT
    wav_audio = pcm_to_wav(raw_pcm)
    try:
        user_text = client.audio.transcriptions.create(
            file=("audio.wav", wav_audio),
            model="whisper-large-v3-turbo",
            response_format="text"
        ).strip()
    except Exception as e:
        print("❌ STT error:", e)
        return "STT error", 500

    print(f"👂 Heard: '{user_text}'")

    if not user_text:
        return "Empty transcription", 200

    # Run agent
    run_agent(user_text, wake_word)
    return "OK", 200


@app.route("/update-sensor", methods=["POST"])
def update_sensor():
    """ESP32_A posts sensor readings here periodically."""
    global front_distance, back_distance
    data = request.get_json()
    if not data:
        return "Bad request", 400
    front_distance = data.get("front", front_distance)
    back_distance  = data.get("back",  back_distance)
    print(f"📡 Sensors updated — Front: {front_distance}cm | Back: {back_distance}cm")
    return "OK", 200


@app.route("/status")
def status():
    return jsonify({"ready": audio_ready})


@app.route("/get-audio-response")
def get_audio():
    global latest_audio, audio_ready
    if not audio_ready:
        return "Not ready", 404
    audio_ready = False
    print("➡️ Sending audio to ESP32_B")
    return Response(latest_audio, mimetype="application/octet-stream")


@app.route("/sensor-status")
def sensor_status():
    return jsonify({"front": front_distance, "back": back_distance})


# ================= RUN =================
if __name__ == "__main__":
    print("🤖 ARIA Server starting...")
    print(f"📡 Listening on port 5000")
    app.run(host="0.0.0.0", port=5000, debug=False)
