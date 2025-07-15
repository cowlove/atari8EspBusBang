
<!-- Improved compatibility of back to top link: See: https://github.com/othneildrew/Best-README-Template/pull/73 -->
<a id="readme-top"></a>
<!--
*** Thanks for checking out the Best-README-Template. If you have a suggestion
*** that would make this better, please fork the repo and create a pull request
*** or simply open an issue with the tag "enhancement".
*** Don't forget to give the project a star!
*** Thanks again! Now go create something AMAZING! :D
-->



<!-- PROJECT SHIELDS -->
<!--
*** I'm using markdown "reference style" links for readability.
*** Reference links are enclosed in brackets [ ] instead of parentheses ( ).
*** See the bottom of this document for the declaration of the reference variables
*** for contributors-url, forks-url, etc. This is an optional, concise syntax you may use.
*** https://www.markdownguide.org/basic-syntax/#reference-style-links
-->
[![Contributors][contributors-shield]][contributors-url]
[![Forks][forks-shield]][forks-url]
[![Stargazers][stars-shield]][stars-url]
[![Issues][issues-shield]][issues-url]
[![project_license][license-shield]][license-url]
[![LinkedIn][linkedin-shield]][linkedin-url]

<!-- PROJECT LOGO -->
<br />
<div>
<pre align="center">
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@     _   _             _  ___  _     _ _        _____ ____  ____ _________   @
@    / \ | |_ __ _ _ __(_)( _ )| |__ (_) |_     | ____/ ___||  _ \___ /___ \  @
@   / _ \| __/ _` | '__| |/ _ \| '_ \| | __|    |  _| \___ \| |_) ||_ \ __) | @
@  / ___ \ || (_| | |  | | (_) | |_) | | |_     | |___ ___) |  __/___) / __/  @
@ /_/   \_\__\__,_|_|  |_|\___/|_.__/|_|\__|    |_____|____/|_|  |____/_____| @
@                ____            ____                    _ _ _ _              @
@               | __ ) _   _ ___| __ )  __ _ _ __   __ _| | | | |             @
@               |  _ \| | | / __|  _ \ / _` | '_ \ / _` | | | | |             @
@               | |_) | |_| \__ \ |_) | (_| | | | | (_| |_|_|_|_|             @
@               |____/ \__,_|___/____/ \__,_|_| |_|\__, (_|_|_|_)             @
@                                                  |___/                      @
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
</pre>

<!-- ABOUT THE PROJECT -->
## About The Project


A common ESP32S3 dev board can directly bit-bang the 
Atari 8-bit memory bus through the PBI port, with no hardware, FPGAs, bus buffering, or level shifters needed.  It can:
* Handle all RAM access using the ESP32 SRAM memory, supporting various memory expansion schemes on the Atari up to a couple hundred KB
* Perform fast "DMA" IO to and from disk images in ESP32 flash using any stock Atari OS ROM and disk IO. 
* Implement various PBI device driver ROMs and ESP32 backends to interface the Atari to the outside world with WiFi, flash, serial, ESP32 GPIOs, or anything else you can dream up with the ESP32. 
* Capture complete Atari memory bus traces with multiple complex trigger filters, preroll, and total sample depth of 2M samples.  

The same zero-component* PCB has different edges that can:
* Plug into the 600/800XL PBI port.
* Plug into the 65/130XE ECI port.
* Plug into any model's cartridge port and emulate a library of cartrige ROMs from the ESP32 flash.  
(* Zero components except, of course, for the ESP32 and a 50-pin card edge PBI connector)

<!-- PROJECT INFO -->

<div>
    <br />
    <a href="https://github.com/cowlove/atari8EspBusBang"><strong>Explore the docs »</strong></a>
    <br />
    <a href="https://github.com/cowlove/atari8EspBusBang">View Demo</a>
    &middot;
    <a href="https://github.com/cowlove/atari8EspBusBang/issues/new?labels=bug&template=bug-report---.md">Report Bug</a>
    &middot;
    <a href="https://github.com/cowlove/atari8EspBusBang/issues/new?labels=enhancement&template=feature-request---.md">Request Feature</a>
</div>

<!-- TABLE OF CONTENTS -->
<details>
  <summary>Table of Contents</summary>
  <ol>
    <li>
        <li><a href="#built-with">Built With</a></li>
      </ul>
    </li>
    <li>
      <a href="#getting-started">Getting Started</a>
      <ul>
        <li><a href="#prerequisites">Prerequisites</a></li>
        <li><a href="#installation">Installation</a></li>
      </ul>
    </li>
    <li><a href="#usage">Usage</a></li>
    <li><a href="#roadmap">Roadmap</a></li>
    <li><a href="#contributing">Contributing</a></li>
    <li><a href="#license">License</a></li>
    <li><a href="#contact">Contact</a></li>
    <li><a href="#acknowledgments">Acknowledgments</a></li>
  </ol>
</details>




### Built With

* [![mak][makeEspArduino]][makeEspArduino-url]
* [![Espress][arduino-esp32]][arduino-esp32-url]

<p align="right">(<a href="#readme-top">back to top</a>)</p>



<!-- GETTING STARTED -->
## Getting Started

This is an example of how you may give instructions on setting up your project locally.
To get a local copy up and running follow these simple example steps.

### Prerequisites

This is an example of how to list things you need to use the software and how to install them.
* npm
  ```sh
  npm install npm@latest -g
  ```

### Installation

1. Get a free API Key at [https://example.com](https://example.com)
2. Clone the repo
   ```sh
   git clone https://github.com/cowlove/atari8EspBusBang.git
   ```
3. Install NPM packages
   ```sh
   npm install
   ```
4. Enter your API in `config.js`
   ```js
   const API_KEY = 'ENTER YOUR API';
   ```
5. Change git remote url to avoid accidental pushes to base project
   ```sh
   git remote set-url origin cowlove/atari8EspBusBang
   git remote -v # confirm the changes
   ```

<p align="right">(<a href="#readme-top">back to top</a>)</p>



<!-- USAGE EXAMPLES -->
## Usage

Use this space to show useful examples of how a project can be used. Additional screenshots, code examples and demos work well in this space. You may also link to more resources.

_For more examples, please refer to the [Documentation](https://example.com)_

<p align="right">(<a href="#readme-top">back to top</a>)</p>



<!-- ROADMAP -->
## Roadmap

- [ ] Feature 1
- [ ] Feature 2
- [ ] Feature 3
    - [ ] Nested Feature

See the [open issues](https://github.com/cowlove/atari8EspBusBang/issues) for a full list of proposed features (and known issues).

<p align="right">(<a href="#readme-top">back to top</a>)</p>



<!-- CONTRIBUTING -->
## Contributing

Contributions are what make the open source community such an amazing place to learn, inspire, and create. Any contributions you make are **greatly appreciated**.

If you have a suggestion that would make this better, please fork the repo and create a pull request. You can also simply open an issue with the tag "enhancement".
Don't forget to give the project a star! Thanks again!

1. Fork the Project
2. Create your Feature Branch (`git checkout -b feature/AmazingFeature`)
3. Commit your Changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the Branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

<p align="right">(<a href="#readme-top">back to top</a>)</p>

### Top contributors:

<a href="https://github.com/cowlove/atari8EspBusBang/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=cowlove/atari8EspBusBang" alt="contrib.rocks image" />
</a>



<!-- LICENSE -->
## License

Distributed under the GPLv2. See `LICENSE.txt` for more information.

<p align="right">(<a href="#readme-top">back to top</a>)</p>



<!-- CONTACT -->
## Contact

Jim Evans - jim@vheavy.com

Project Link: [https://github.com/cowlove/atari8EspBusBang](https://github.com/cowlove/atari8EspBusBang)

<p align="right">(<a href="#readme-top">back to top</a>)</p>



<!-- ACKNOWLEDGMENTS -->
## Acknowledgments

* []()
* []()
* []()

<p align="right">(<a href="#readme-top">back to top</a>)</p>



<!-- MARKDOWN LINKS & IMAGES -->
<!-- https://www.markdownguide.org/basic-syntax/#reference-style-links -->
[contributors-shield]: https://img.shields.io/github/contributors/cowlove/atari8EspBusBang.svg?style=for-the-badge
[contributors-url]: https://github.com/cowlove/atari8/graphs/contributors
[forks-shield]: https://img.shields.io/github/forks/cowlove/atari8EspBusBang.svg?style=for-the-badge
[forks-url]: https://github.com/cowlove/atari8EspBusBang/network/members
[stars-shield]: https://img.shields.io/github/stars/cowlove/atari8EspBusBang.svg?style=for-the-badge
[stars-url]: https://github.com/cowlove/atari8EspBusBang/stargazers
[issues-shield]: https://img.shields.io/github/issues/cowlove/atari8EspBusBang.svg?style=for-the-badge
[issues-url]: https://github.com/cowlove/atari8EspBusBang/issues
[license-shield]: https://img.shields.io/github/license/cowlove/atari8EspBusBang.svg?style=for-the-badge
[license-url]: https://github.com/cowlove/atari8EspBusBang/blob/master/LICENSE.txt
[linkedin-shield]: https://img.shields.io/badge/-LinkedIn-black.svg?style=for-the-badge&logo=linkedin&colorB=555
[linkedin-url]: https://linkedin.com/in/linkedin_username
[product-screenshot]: images/screenshot.png
[makeEspArduino]: https://img.shields.io/badge/makeEspArduino-blue
[makeEspArduino-url]: https://github.com/plerup/makeEspArduino
[arduino-esp32]: https://img.shields.io/badge/Espressif_ESP32_Arduino_Library-blue
[arduino-esp32-url]: https://github.com/espressif/arduino-esp32
