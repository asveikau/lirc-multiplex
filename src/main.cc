/*
 Copyright (C) 2021, 2026 Andrew Sveikauskas

 Permission to use, copy, modify, and distribute this software for any
 purpose with or without fee is hereby granted, provided that the above
 copyright notice and this permission notice appear in all copies.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>

#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

#include <common/error.h>
#include <common/logger.h>
#include <common/checkatoi.h>

#include <common/c++/new.h>
#include <common/c++/refcount.h>

#include <pollster/sockapi.h>
#include <pollster/pollster.h>

class LineProcessor
{
   std::vector<char> pendingSocketData;

public:

   virtual ~LineProcessor() {}

   virtual void
   ProcessLine(char *buf, error *err) = 0;

   void
   OnSocketData(const void *buf, size_t len, error *err)
   {
      if (pendingSocketData.size())
      {
         Append(buf, len, err);
         ERROR_CHECK(err);

         char *end = ProcessLines(pendingSocketData.data(), pendingSocketData.size(), err);
         ERROR_CHECK(err);
         if (end != pendingSocketData.data())
         {
            auto delta = end - pendingSocketData.data();
            pendingSocketData.erase(pendingSocketData.begin(), pendingSocketData.begin()+delta);
         }
      }
      else
      {
         char *buf_s = (char*)buf;
         auto next = ProcessLines(buf_s, len, err);
         ERROR_CHECK(err);
         auto newLen = len - (next - buf_s);
         if (newLen)
         {
            Append(next, newLen, err);
            ERROR_CHECK(err);
         }
      }
   exit:;
   }

private:

   void
   Append(const void *buf, size_t len, error *err)
   {
      try
      {
         pendingSocketData.insert(
            pendingSocketData.end(),
            (const char*)buf,
            (const char*)buf + len
         );
      }
      catch (const std::bad_alloc &)
      {
         error_set_nomem(err);
      }
   }

   char *
   FindNewline(char *buf, size_t len)
   {
      while (len && *buf != '\n')
      {
         ++buf;
         --len;
      }
      return len && *buf == '\n' ? buf : nullptr;
   }

   char *
   ProcessLines(char *buf, size_t len, error *err)
   {
      char *p;
      while ((p = FindNewline(buf, len)))
      {
         *p++ = 0;
         ProcessLine(buf, err);
         ERROR_CHECK(err);
         len -= (p - buf);
         buf = p;
      }
   exit:
      return buf;
   }
};

struct SockBase : public std::enable_shared_from_this<SockBase>, public LineProcessor
{
   std::shared_ptr<pollster::StreamSocket> sock;

   virtual ~SockBase() {}

   void
   Init(const std::shared_ptr<pollster::StreamSocket> &sock, error *err)
   {
      this->sock = sock;

      auto This = std::weak_ptr<SockBase>(shared_from_this());

      sock->on_closed = [This] (error *err) -> void
      {
         if (auto p = This.lock())
            p->OnClosed(err);
      };
      sock->on_error = sock->on_closed;
      sock->on_recv = [This] (const void *buf, size_t len, error *err) -> void
      {
         if (auto p = This.lock())
            p->OnSocketData(buf, len, err);
      };
   }

   virtual void
   OnClosed(error *err)
   {
   }
};

static bool
CheckCommand(const char *buf, const char *cmd)
{
   size_t l = strlen(cmd);
   return (!strncasecmp(buf, cmd, l) &&
           (buf[l] == 0 || buf[l] == ' '));
}

static void
Broadcast(const char *cmd, size_t l, error *err);

struct Device;

static void
OnClosed(Device *);

struct Device : public SockBase
{
   enum
   {
      None,
      AwaitBegin,
      AwaitEnd,
   } State;

   std::vector<char> cmdbuf;

   typedef std::function<void(const char*,size_t,error*)> ResponseCallback;
   struct PendingResponse
   {
#ifdef DELAYED_WRITE
      std::vector<char> cmd;
#endif
      ResponseCallback cb;
   };
   std::vector<PendingResponse> responseCallbacks;

   Device() : State(None) {}
   virtual ~Device() {}

   void
   Init(const char *dev, error *err)
   {
      std::shared_ptr<pollster::StreamSocket> sock;

      common::New(sock, err);
      ERROR_CHECK(err);

      SockBase::Init(sock, err);
      ERROR_CHECK(err);

      sock->ConnectUnixDomain(dev);
   exit:;
   }

   void
   ProcessLine(char *buf, error *err)
   {
      auto insert = [&] (const char *p, error *err) -> void
      {
         try
         {
            cmdbuf.insert(cmdbuf.end(), p, p+strlen(p));
            cmdbuf.push_back('\n');
         }
         catch (const std::bad_alloc&)
         {
            error_set_nomem(err);
         }
      };
      switch (State)
      {
      case None:
         Broadcast(buf, strlen(buf), err);
         ERROR_CHECK(err);
         Broadcast("\n", 1, err);
         ERROR_CHECK(err);
         break;
      case AwaitBegin:
         if (CheckCommand(buf, "BEGIN"))
         {
            State = AwaitEnd;

            insert(buf, err);
            ERROR_CHECK(err);
         }
         else
         {
            Broadcast(buf, strlen(buf), err);
            ERROR_CHECK(err);
         }
         break;
      case AwaitEnd:
         insert(buf, err);
         ERROR_CHECK(err);
         if (CheckCommand(buf, "END"))
         {
            State = None;

            static const char sighup[] = "\nSIGHUP\n";
            if (std::search(cmdbuf.begin(), cmdbuf.end(), sighup, sighup+sizeof(sighup)-1) != cmdbuf.end())
            {
               // SIGHUP needs to bypass the normal response callback
               // mechanism, it just gets treated as unsolicted.
               //
               Broadcast(cmdbuf.data(), cmdbuf.size(), err);
               ERROR_CHECK(err);
            }
            else if (responseCallbacks.size())
            {
               auto cb = std::move(responseCallbacks[0]);
               responseCallbacks.erase(responseCallbacks.begin());
               cb.cb(cmdbuf.data(), cmdbuf.size(), err);
               ERROR_CHECK(err);
            }
            cmdbuf.clear();

            if (responseCallbacks.size())
            {
#ifdef DELAYED_WRITE
               auto &cb = responseCallbacks[0];
               sock->Write(cb.cmd.data(), cb.cmd.size());
#endif
               State = AwaitBegin;
            }
         }
         break;
      }
   exit:;
   }

   void
   WriteCommand(
      const char *cmd,
      const ResponseCallback &response,
      error *err
   )
   {
      try
      {
         PendingResponse resp;
#ifdef DELAYED_WRITE
         resp.cmd.insert(resp.cmd.end(), cmd, cmd+strlen(cmd));
         resp.cmd.push_back('\n');
#endif
         resp.cb = std::move(response);
         responseCallbacks.push_back(resp);

#ifdef DELAYED_WRITE
         if (responseCallbacks.size() == 1)
            sock->Write(resp.cmd.data(), resp.cmd.size());
#else
         sock->Write(cmd, strlen(cmd));
         sock->Write("\n", 1);
#endif
         if (responseCallbacks.size() == 1)
            State = AwaitBegin;
      }
      catch (const std::bad_alloc &)
      {
         ERROR_SET(err, nomem);
      }
   exit:;
   }

   void
   OnClosed(error *err)
   {
      auto cb = std::vector<PendingResponse>();
      std::swap(responseCallbacks, cb);

      ::OnClosed(this);

      static const char resp[] = "BEGIN\nERROR\nDATA The remote connection closed before receiving a response.\nEND\n";
      for (auto &p : cb)
      {
         error err;
         p.cb(resp, sizeof(resp)-1, &err);
      }
   }
};

struct DeviceRef
{
   const char *path;
   std::weak_ptr<Device> dev;

   DeviceRef() : path(nullptr) {}

   void
   Open(std::shared_ptr<Device> &out, error *err)
   {
      if (!(out = dev.lock()))
      {
         if (!path)
            ERROR_SET(err, unknown, "device path not specified");

         common::New(out, err);
         ERROR_CHECK(err);
         out->Init(path, err);
         ERROR_CHECK(err);

         dev = out;
      }
      exit:;
   }
};

static DeviceRef rxDevice, txDevice;

struct Client;
static void OnClosed(Client *cli);

struct Client : public SockBase
{
   std::shared_ptr<Device> rx, tx;

   void
   Init(const std::shared_ptr<pollster::StreamSocket> &sock, error *err)
   {
      SockBase::Init(sock, err);
      ERROR_CHECK(err);

      rxDevice.Open(rx, err);
      ERROR_CHECK(err);
   exit:;
   }

   void
   ProcessLine(char *buf, error *err)
   {
      static const char *txCmds [] =
      {
         "SEND_ONCE", "SEND_START", "SEND_STOP",
         "SET_TRANSMITTERS", "SIMULATE",
         nullptr
      };
      DeviceRef *devRef = &rxDevice;
      std::shared_ptr<Device> *dev = &rx;
      auto p = txCmds;
      while (*p)
      {
         if (CheckCommand(buf, *p))
         {
            devRef = &txDevice;
            dev = &tx;
            break;
         }
         ++p;
      }

      auto This = std::weak_ptr<SockBase>(shared_from_this());

      if (!dev->get())
      {
         devRef->Open(*dev, err);
         ERROR_CHECK(err);
      }

      if (!*dev)
         ERROR_SET(err, unknown, "Device not open");

      (*dev)->WriteCommand(
         buf,
         [This] (const char *buf, size_t n, error *err) -> void
         {
            if (auto p = This.lock())
            {
               if (p->sock)
                  p->sock->Write(buf, n);
            }
         },
         err
      );
      ERROR_CHECK(err);
   exit:;
   }

   void
   OnDeviceClosed(Device *dev)
   {
      if (tx.get() == dev)
         tx = nullptr;
      if (rx.get() == dev)
      {
         rx = nullptr;

         // Reopen after a delay.
         //
         error err;
         std::weak_ptr<SockBase> This = shared_from_this();
         pollster::sleep(
            nullptr,
            1000,
            [This, this] (error *err) -> void
            {
               if (auto p = This.lock())
               {
                  if (!rx.get())
                     rxDevice.Open(rx, err);
               }
            },
            &err
         );
      }
   }

   void
   OnClosed(error *err)
   {
      ::OnClosed(this);
   }
};

static std::vector<std::shared_ptr<Client>> clients;

static void
Broadcast(const char *cmd, size_t l, error *err)
{
   for (auto cli : clients)
   {
      cli->sock->Write(cmd, l);
   }
}

static void
OnClosed(Device *dev)
{
   for (auto &cli : clients)
   {
      cli->OnDeviceClosed(dev);
   }
}

static void
OnClosed(Client *cli)
{
   auto pred =
      [cli] (const std::shared_ptr<Client> &p) -> bool { return p.get() == cli; };

#if 0
   std::erase_if(clients, pred);
#else
   clients.erase(std::remove_if(clients.begin(), clients.end(), pred), clients.end());
#endif
}

#define VALID_PORT(PORT) \
           ((PORT) > 0 && (PORT) <= 0xffff)

static
bool
ParsePath(
   const char *s,
   const std::function<void(const char*)> &onUnix,
   const std::function<void(int)> &onTcp
)
{
   if (!strncmp(s, "tcp:", 4))
   {
      int port = 0;

      if (!check_atoi(s+4, &port) || !VALID_PORT(port))
         return false;

      onTcp(port);
   }
   else
   {
      if (!strncmp(s, "unix:", 5))
         s += 5;

      onUnix(s);
   }

   return true;
}

int
main(int argc, char **argv)
{
   error err;
   common::Pointer<pollster::waiter> eventLoop;
   pollster::StreamServer server;
   std::vector<const char *> unixSockPaths;
   const char *chmodArg = nullptr;
   char *chownArg = nullptr;
   uid_t uid = -1;
   gid_t gid = -1;
   mode_t mode = 0666;

   log_register_callback(
      [] (void *np, const char *p) -> void { fputs(p, stderr); },
      nullptr
   );

   pollster::create(eventLoop.GetAddressOf(), &err);
   ERROR_CHECK(&err);

   pollster::set_common_queue(eventLoop.Get());

   for (int i=1; i<argc; ++i)
   {
      auto arg = argv[i];
      if (!strcmp(arg, "-server"))
      {
         if (!server.on_client)
         {
            server.on_client = [] (const std::shared_ptr<pollster::StreamSocket> &fd, error *err) -> void
            {
               try
               {
                  auto cli = std::make_shared<Client>();

                  cli->Init(fd, err);
                  ERROR_CHECK(err);

                  clients.push_back(cli);
               }
               catch (const std::bad_alloc &)
               {
                  ERROR_SET(err, nomem);
               }
            exit:;
            };
         }

         if (i+1 >= argc ||
             !ParsePath(
               argv[i+1],
               [&] (const char *path) -> void
               {
                  server.AddUnixDomain(path, &err);
                  ERROR_CHECK(&err);
                  try
                  {
                     unixSockPaths.push_back(path);
                  }
                  catch (const std::bad_alloc &)
                  {
                     ERROR_SET(&err, nomem);
                  }
               exit:;
               },
               [&] (int port) -> void { server.AddPort(port, &err); }
             ))
         {
            ERROR_SET(&err, unknown, "\nusage: -server unixpath or -server tcp:<port>");
         }
         ERROR_CHECK(&err);
         ++i;
      }
      else if (!strcmp(arg, "-rx"))
      {
         if (i+1 >= argc)
            ERROR_SET(&err, unknown, "\nusage: -rx unixpath");
         rxDevice.path = argv[i+1];
         ++i;
      }
      else if (!strcmp(arg, "-tx"))
      {
         if (i+1 >= argc)
            ERROR_SET(&err, unknown, "\nusage: -tx unixpath");
         txDevice.path = argv[i+1];
         ++i;
      }
      else if (!strcmp(arg, "-chmod"))
      {
         if (i+1 >= argc)
            ERROR_SET(&err, unknown, "\nusage: -chmod <perms>");
         chmodArg = argv[i+1];
         ++i;
      }
      else if (!strcmp(arg, "-chown"))
      {
         if (i+1 >= argc)
            ERROR_SET(&err, unknown, "\nusage: -chown user[:group]");
         chownArg = argv[i+1];
         ++i;
      }
   }

   if (!server.on_client || !rxDevice.path)
   {
      ERROR_SET(&err, unknown,
         "\nusage: lirc-multiplex -server <unixpath | tcp:port>\n"
           "                      -rx <unixpath> "
                                 "-tx <unixpath>\n"
          "                      [-chmod <permissions>]\n"
          "                      [-chown <user[:group]>]\n\n"
         "Combines a receive LIRC socket with a transmit LIRC socket, into the socket specified by -server.\n"
         "Optionally, you can set permissions of any Unix domain sockets created with -chmod and -chown options"
      );
   }

   if (chmodArg)
   {
      char *end = nullptr;
      mode = strtoll(chmodArg, &end, 8);
      if (!end || end == chmodArg || *end)
         ERROR_SET(&err, unknown, "Error parsing chmod arg");
   }

   if (chownArg)
   {
      char *colon = strchr(chownArg, ':');
      if (colon)
      {
         *colon++ = 0;
         struct group *gr = getgrnam(colon);
         if (!gr)
            ERROR_SET(&err, unknown, "chown: Could not find group");
         gid = gr->gr_gid;
      }
      struct passwd *p = getpwnam(chownArg);
      if (!p)
         ERROR_SET(&err, unknown, "chown: Could not find user");
      uid = p->pw_uid;
   }

   if (chmodArg || chownArg)
   {
      for (auto path : unixSockPaths)
      {
         if (chownArg && chown(path, uid, gid))
            ERROR_SET(&err, errno, errno);
         if (chmodArg && chmod(path, mode))
            ERROR_SET(&err, errno, errno);
      }
   }

   unixSockPaths.resize(0);

   for (;;)
   {
      eventLoop->exec(&err);
      ERROR_CHECK(&err);
   }

   return 0;
exit:
   return 1;
}
