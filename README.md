# web-server
a simple web server using epoll.
### 简介
web服务器根据《深入理解计算机系统》书中的相关代码学习而来。具有如下特点：
1. 利用多线程技术
2. 使用了带缓冲的输入输出函数RIO read write来对网络文件描述符进行读写
3. 支持静态文件.php,.html,支持动态程序
4. 可以处理信号，统一信号源在main loop中处理
5. 利用linux零拷贝技术，sendfile传送静态文件，减少内核态到用户态数据的复制次数
6. io复用利用epoll
这是早期学习c语言的代码。现在学习node。其中对于如何使用epoll。此代码具有学习理解之用。
node中libuv对于epoll的使用以及线程池对于文件的操作与这里的代码基本原理都是一致的。

### 使用说明
编译：gcc -o server -lpthread csapp.c sbuf.c server.c
命令行输入：./server.c 80 &
测试：ab -n1000 -c10  http://127.0.0.1/test.php


