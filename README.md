# KliveTech Ecosystem
A library which I can import into my Arduino/robotics projects, to automatically connect to Omnipotent and allow for easy interactions.

# What this does/why I have made it

One of my secondary hobbies alongside programming is making tech, tech that I either find really cool or just stuff to make my life easier.

While writing code for the ESP32s I use within my gadgets, I find myself repeating the same process in making a web server on the ESP so that I can control it remotely. So I have made a library which allows me to automatically connect to my [Omnipotent](https://github.com/Klivess/Omnipotent) bot and define actions through functions in only a couple of lines of code.

# Why this is awesome
1. It automatically connects to [Omnipotent](https://github.com/Klivess/Omnipotent) (my automation bot, which acts a hub for my gadgets as well) via Bluetooth on my home server, meaning I can upload the code and its ready to go.
2. It is super fast to interface with, and requires very little processing power on the ESP32 opposed to setting up a usually unreliable web server on the ESP32.
3. I can control my tech ANYWHERE through [Omnipotent](https://github.com/Klivess/Omnipotent), with no work required within the microcontroller.
4. It is super easy to enable my tech to be controlled remotely, typically through my [Klives Management](https://github.com/Klivess/Klives-Management-Website) website, as in execution, this only requires 2-3 lines in the Arduino project to set up. This makes it much quicker for me to make new stuff instead of wasting time writing/debugging microcontroller code.
5. I can update this library to increase the functionality across all of my tech, such as Over-The-Air updating.

# Code that lets this ecosystem work on Omnipotent
https://github.com/Klivess/Omnipotent/blob/master/Omnipotent/Services/KliveTechHub/KliveTechHub.cs

# Example of how I use this

![image](https://github.com/user-attachments/assets/7777e260-daf3-47cc-85ee-7aa47fcc0ee9)

# What I want to add in the future
1. OAT (Over The Air) firmware updates from Omnipotent
2. Syncing data from microcontroller to Omnipotent

# Problems I need to fix
1. Use of std::string in this library means that program memory gets sucked up fast, need to switch all string operations to memory-safe const char* operations.
