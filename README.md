# A simple STOMP broker

A simple implementation of the STOMP protocol in C. This
project uses OpenBSD's libevent which is included in base.

## What needs to be done

Everything... A crude implementation of CONNECT and DISCONNECT
has been done, however it is far from complete.

Currently lacking a test-suite to test all aspects of the STOMP
protocol. 

This project is not currently portable, I believe [oconfigure](https://github.com/kristapsdz/oconfigure)
 could be the answer to this.

## Future plans / goals

* Privilege seperated processes for security, as seen on httpd, relayd and opensmtpd.
* Small and simple configuration file syntax.
* A command-line tool for control and statistics (stompctl).
* Speedy and efficient, but still readable. 
* Portable.

## License

Refer to the [LICENSE](LICENSE) file for details.
