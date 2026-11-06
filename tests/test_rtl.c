#include "rtl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Every descriptor and Ethernet frame here is a synthetic memory fixture.
 * None was captured from hardware. Byte oracles are independent literals. */
static void put32(uint8_t *p,uint32_t v)
{ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static void codec(enum rtl_chip chip)
{
    uint8_t a[61],b[73],buf[512],rx[512],sentinel[512];
    struct rtl_frame f[2],bridge[2]; struct rtl_frame_view v[2],old[2];
    struct rtl_format fmt; size_t n,k,r,i,aligned;
    const uint8_t tx1[]={61,0,0,0xc0,0,0,0,0};
    const uint8_t tx2[]={61,0,0,0xc0,0,0,0,0,0xd0,3,0,0,0,0,0,0xa8};
    memset(a,0x51,sizeof a);memset(b,0x62,sizeof b);
    f[0]=(struct rtl_frame){a,sizeof a};f[1]=(struct rtl_frame){b,sizeof b};
    assert(rtl_get_format(chip,&fmt)==0);
    memset(buf,0xa5,sizeof buf);
    assert(rtl_tx_encode(chip,f,2,buf,sizeof buf,&n)==0);
    assert(!memcmp(buf,chip==RTL_8159?tx2:tx1,fmt.tx_header));
    aligned=(fmt.tx_header+61+fmt.tx_align-1)&~(fmt.tx_align-1);
    assert(n==aligned+fmt.tx_header+73);
    for(i=fmt.tx_header+61;i<aligned;i++) assert(buf[i]==0);
    assert(rtl_tx_decode(chip,buf,n,v,2,&k)==0 && k==2);
    assert(v[0].len==61 && v[1].len==73 && !memcmp(v[1].data,b,73));
    for(i=0;i<2;i++) bridge[i]=(struct rtl_frame){v[i].data,v[i].len};
    assert(rtl_rx_fixture_encode(chip,bridge,2,rx,sizeof rx,&r)==0);
    if(chip==RTL_8159) assert(rx[0]==0 && rx[1]==0 && rx[2]==0x82 && rx[3]==0);
    else assert(rx[0]==65 && rx[1]==0);
    assert(rtl_rx_decode(chip,rx,r,v,2,&k)==0 && k==2);
    assert(v[0].len==61 && v[1].len==73 && !memcmp(v[0].data,a,61));
    assert(rtl_rx_decode(chip,rx,r,NULL,0,&k)==RTL_ENOSPC && k==2);
    memset(v,0x39,sizeof v);memcpy(old,v,sizeof v);
    assert(rtl_rx_decode(chip,rx,r,v,1,&k)==RTL_ENOSPC && k==2);
    assert(!memcmp(v,old,sizeof v));
    /* Malformed later record must not publish a good prefix. */
    memset(v,0xa6,sizeof v);memcpy(old,v,sizeof v);
    assert(rtl_rx_decode(chip,rx,r-1,v,2,&k)==RTL_ETRUNC && k==0);
    assert(!memcmp(v,old,sizeof v));
    assert(rtl_tx_decode(chip,buf,n-1,v,2,&k)==RTL_ETRUNC);
    /* No/partial/full final alignment padding accepted, but one extra byte
     * starts a truncated descriptor and must fail. Nonzero padding is legal. */
    assert(rtl_rx_fixture_encode(chip,f,1,rx,sizeof rx,&r)==0);
    aligned=(r+fmt.rx_align-1)&~(fmt.rx_align-1);
    memset(rx+r,0xfa,aligned-r+1);
    for(i=r;i<=aligned;i++) assert(rtl_rx_decode(chip,rx,i,v,2,&k)==0 && k==1);
    assert(rtl_rx_decode(chip,rx,aligned+1,v,2,&k)==RTL_ETRUNC);
    assert(rtl_tx_encode(chip,f,1,buf,sizeof buf,&n)==0);
    aligned=(n+fmt.tx_align-1)&~(fmt.tx_align-1);memset(buf+n,0xfa,aligned-n+1);
    for(i=n;i<=aligned;i++) assert(rtl_tx_decode(chip,buf,i,v,2,&k)==0 && k==1);
    assert(rtl_tx_decode(chip,buf,aligned+1,v,2,&k)==RTL_ETRUNC);
    /* Reject offload profile, missing first/last flags, and v2 length/signature. */
    buf[3]^=0x80;assert(rtl_tx_decode(chip,buf,n,v,2,&k)==RTL_EFORMAT);buf[3]^=0x80;
    buf[4]=1;assert(rtl_tx_decode(chip,buf,n,v,2,&k)==RTL_EFORMAT);buf[4]=0;
    if(chip==RTL_8159) {
        buf[8]^=1;assert(rtl_tx_decode(chip,buf,n,v,2,&k)==RTL_EFORMAT);buf[8]^=1;
        buf[15]^=1;assert(rtl_tx_decode(chip,buf,n,v,2,&k)==RTL_EFORMAT);buf[15]^=1;
    }
    /* Wrong chip layouts are rejected for these concrete fixtures. RX does not
     * carry an unambiguous chip signature; caller still selects trusted format. */
    assert(rtl_tx_decode(chip==RTL_8159?RTL_8156B:RTL_8159,buf,n,v,2,&k)!=0);
    assert(rtl_rx_decode(chip==RTL_8159?RTL_8156B:RTL_8159,rx,r,v,2,&k)!=0);
    /* VLAN metadata byte order differs; payload bytes are not silently tagged. */
    if(chip==RTL_8159) { rx[0]|=8;put32(rx+4,0x23010000u); }
    else put32(rx+4,0x00012301u);
    assert(rtl_rx_decode(chip,rx,r,v,2,&k)==0 && v[0].vlan_present && v[0].vlan_tci==0x123);
    put32(rx,chip==RTL_8159?17u<<17:17u);
    assert(rtl_rx_decode(chip,rx,r,v,2,&k)==RTL_EFORMAT);
    put32(rx,chip==RTL_8159?0x7fffu<<17:0x7fffu);
    assert(rtl_rx_decode(chip,rx,r,v,2,&k)==RTL_EFORMAT);
    memset(buf,0xa7,sizeof buf);memcpy(sentinel,buf,sizeof buf);
    assert(rtl_tx_encode(chip,f,2,buf,1,&n)==RTL_ENOSPC && n==0);
    assert(!memcmp(buf,sentinel,sizeof buf));
    f[1].len=SIZE_MAX;assert(rtl_tx_encode(chip,f,2,buf,sizeof buf,&n)==RTL_EINVAL);
    assert(!memcmp(buf,sentinel,sizeof buf));
    f[1].len=sizeof b;
    assert(rtl_tx_encode(chip,f,SIZE_MAX,buf,sizeof buf,&n)==RTL_ENOSPC);
    f[0].len=14;
    assert(rtl_tx_encode(chip,f,1,buf,sizeof buf,&n)==0);
    assert(rtl_tx_decode(chip,buf,n,v,2,&k)==0 && v[0].len==60);
    for(i=14;i<60;i++) assert(v[0].data[i]==0);
    f[0].len=13;assert(rtl_tx_encode(chip,f,1,buf,sizeof buf,&n)==RTL_EINVAL);
    assert(rtl_rx_decode(chip,NULL,0,v,2,&k)==RTL_EINVAL);
    assert(rtl_rx_decode(chip,rx,1,v,2,&k)==RTL_ETRUNC);
    /* Alignment is relative to aggregate offset, not host pointer address. */
    f[0].len=sizeof a;
    assert(rtl_tx_encode(chip,f,2,buf+1,sizeof buf-1,&n)==0);
    assert(rtl_tx_decode(chip,buf+1,n,v,2,&k)==0 && k==2);
    assert(rtl_rx_fixture_encode(chip,f,2,rx+1,sizeof rx-1,&r)==0);
    assert(rtl_rx_decode(chip,rx+1,r,v,2,&k)==0 && k==2);
}
static void crc_oracle(void)
{
    /* CRC32("123456789") cannot be an Ethernet frame; use the known Ethernet
     * CRC of fourteen zero bytes: 0xd1bb79c7, generated independently with zlib. */
    uint8_t a[14]={0},buf[64];size_t n;
    struct rtl_frame f={a,sizeof a};
    const uint8_t fcs[]={0xc7,0x79,0xbb,0xd1};
    assert(rtl_rx_fixture_encode(RTL_8156B,&f,1,buf,sizeof buf,&n)==0);
    assert(n==42 && !memcmp(buf+38,fcs,4));
}
static void descriptors(void)
{
    uint8_t dev[]={18,1,0,3,0,0,0,9,0xda,0x0b,0x56,0x81,0,1,1,2,3,1};
    uint8_t cfg[]={9,2,44,0,1,1,0,0x80,50,
        9,4,0,0,3,0xff,0xff,0,0,
        7,5,0x81,2,0,4,0, 7,5,0x02,2,0,4,0,
        7,5,0x83,3,8,0,9, 5,0x24,1,2,3};
    /* One interface with two valid alternate settings (bulk, wMaxPacketSize=64). */
    uint8_t alternates[]={9,2,55,0,1,1,0,0x80,50,
        9,4,0,0,2,0xff,0,0,0, 7,5,0x81,2,64,0,0, 7,5,2,2,64,0,0,
        9,4,0,1,2,0xff,0,0,0, 7,5,0x84,2,64,0,0, 7,5,5,2,64,0,0};
    struct rtl_usb_device d;struct rtl_usb_interface v,old;
    enum rtl_chip chip;size_t i; uint16_t version;
    uint8_t setup[8]; const uint8_t expected[]={0xc0,5,0x10,0xe6,0,1,4,0};
    const uint8_t reply[]={0x12,0x34,0x27,0x20};
    assert(rtl_version_request(setup)==0 && !memcmp(setup,expected,8));
    assert(rtl_version_reply(reply,4,&version)==0 && version==0x2020);
    assert(rtl_version_reply(reply,3,&version)==RTL_ETRUNC);
    {
        static const uint8_t phy_expected[8]={0xc0,0x05,0x08,0xe9,0x00,0x01,0x04,0x00};
        const uint8_t phy_reply[]={0x03,0x04,0xff,0xff};
        uint8_t phy_setup[8]; uint16_t status;
        assert(rtl_phy_status_request(phy_setup)==0 && !memcmp(phy_setup,phy_expected,8));
        assert(rtl_phy_status_reply(phy_reply,4,&status)==0 && status==0x0403);
        assert(rtl_phy_status_reply(phy_reply,3,&status)==RTL_ETRUNC);
        assert(rtl_phy_link(status) && rtl_phy_mbps(status)==2500);
        assert(rtl_phy_mbps(0x4002)==10000 && rtl_phy_mbps(0x1002)==5000);
        assert(rtl_phy_mbps(0x0012)==1000 && rtl_phy_mbps(0x000a)==100 && rtl_phy_mbps(0x0006)==10);
        assert(rtl_phy_mbps(0x0002)==0 && rtl_phy_link(0x0002));
        assert(rtl_read_request(phy_setup,RTL_MCU_PLA,0xe908,4)==0 && !memcmp(phy_setup,phy_expected,8));
        assert(rtl_read_request(phy_setup,RTL_MCU_USB,0xd434,RTL_TALLY_LEN)==0);
        assert(phy_setup[2]==0x34 && phy_setup[3]==0xd4 && !phy_setup[4] && !phy_setup[5] && phy_setup[6]==64);
        assert(rtl_read_request(phy_setup,RTL_MCU_PLA,0xe813,4)==RTL_EINVAL);
        assert(rtl_read_request(phy_setup,RTL_MCU_PLA,0xe810,5)==RTL_EINVAL);
        assert(rtl_read_request(phy_setup,RTL_MCU_PLA,0xe810,68)==RTL_EINVAL);
        assert(rtl_byte_write_index(RTL_MCU_PLA,0xe813)==0x0188);
        assert(rtl_byte_write_index(RTL_MCU_PLA,0xe810)==0x0111);
    }
    {
        uint8_t tally[RTL_TALLY_LEN]={0}; struct rtl_tally t;
        tally[0]=5; tally[4]=1;          /* tx_packets */
        tally[8]=7;                      /* rx_packets */
        tally[16]=2;                     /* tx_errors */
        tally[24]=6;                     /* rx_errors */
        tally[28]=9; tally[30]=1;        /* rx_missed, then align_errors */
        tally[60]=3; tally[62]=4;        /* tx_aborted, tx_underrun */
        assert(rtl_tally_reply(tally,sizeof tally,&t)==0);
        assert(t.tx_packets==0x100000005ull && t.rx_packets==7 && t.tx_errors==2);
        assert(t.rx_errors==6 && t.rx_missed==9);
        assert(t.tx_aborted==3 && t.tx_underrun==4);
        assert(rtl_tally_reply(tally,63,&t)==RTL_ETRUNC);
        assert(!rtl_phy_link(0x4000) && !rtl_phy_mbps(0x4000));
    }
    assert(rtl_parse_device(dev,sizeof dev,&d)==0 && d.vid==0xbda && d.pid==0x8156);
    assert(rtl_identify_target(&d,0x7400,&chip)==0 && chip==RTL_8156B);
    assert(rtl_identify_target(&d,0x7410,&chip)==0);
    assert(rtl_identify_target(&d,0x7420,&chip)==RTL_EUNSUPPORTED);
    assert(rtl_identify_target(&d,0x2020,&chip)==RTL_EUNSUPPORTED);
    d.pid=0x815a;assert(rtl_identify_target(&d,0x2020,&chip)==0 && chip==RTL_8159);
    d.vid=0x1234;assert(rtl_identify_target(&d,0x2020,&chip)==RTL_EUNSUPPORTED);
    d.vid=0xbda;d.pid=0x8159;assert(rtl_identify_target(&d,0x2020,&chip)==RTL_EUNSUPPORTED);
    for(i=0;i<18;i++) assert(rtl_parse_device(dev,i,&d)!=0);
    assert(rtl_parse_config(cfg,sizeof cfg,0,0,&v)==0);
    assert(v.bulk_in==0x81 && v.bulk_out==2 && v.interrupt_in==0x83 && v.bulk_in_mps==1024);
    assert(rtl_parse_config(cfg,sizeof cfg,0,1,&v)==RTL_EUNSUPPORTED);
    memset(&v,0x5a,sizeof v);memcpy(&old,&v,sizeof v);
    for(i=0;i<sizeof cfg;i++) {
        assert(rtl_parse_config(cfg,i,0,0,&v)!=0);
        assert(!memcmp(&v,&old,sizeof v));
    }
    cfg[27]=0x81;assert(rtl_parse_config(cfg,sizeof cfg,0,0,&v)==RTL_EFORMAT);cfg[27]=2;
    cfg[13]=2;assert(rtl_parse_config(cfg,sizeof cfg,0,0,&v)==RTL_EFORMAT);cfg[13]=3;
    cfg[18]=0;assert(rtl_parse_config(cfg,sizeof cfg,0,0,&v)==RTL_EFORMAT);cfg[18]=7;
    cfg[22]=0xff;assert(rtl_parse_config(cfg,sizeof cfg,0,0,&v)==RTL_EFORMAT);cfg[22]=0;
    cfg[11]=1;assert(rtl_parse_config(cfg,sizeof cfg,0,0,&v)==RTL_EFORMAT);cfg[11]=0;
    cfg[12]=1;assert(rtl_parse_config(cfg,sizeof cfg,0,1,&v)==RTL_EFORMAT);cfg[12]=0;
    cfg[22]=128;cfg[23]=0; /* 128-byte bulk MPS is not a legal USB speed profile. */
    assert(rtl_parse_config(cfg,sizeof cfg,0,0,&v)==RTL_EFORMAT);
    /* Alternate-setting selection: each alternate owns its own endpoint pair. */
    assert(rtl_parse_config(alternates,sizeof alternates,0,0,&v)==0);
    assert(v.alternate==0 && v.bulk_in==0x81 && v.bulk_out==2 && v.bulk_in_mps==64);
    assert(rtl_parse_config(alternates,sizeof alternates,0,1,&v)==0);
    assert(v.alternate==1 && v.bulk_in==0x84 && v.bulk_out==5 && v.bulk_in_mps==64);
    memset(&v,0x5a,sizeof v);memcpy(&old,&v,sizeof v);
    assert(rtl_parse_config(alternates,sizeof alternates,0,2,&v)==RTL_EUNSUPPORTED);
    assert(!memcmp(&v,&old,sizeof v));
}
static uint32_t rng=0x13579bdfu;
static uint32_t random32(void)
{ rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng; }
static void valid_random(void)
{
    uint8_t data[RTL_MAX_FRAME],tx[40000],rx[40000];
    struct rtl_frame f[4],bridge[4];struct rtl_frame_view v[4];
    unsigned t;size_t i,j,n,r,k;enum rtl_chip chip;
    for(i=0;i<sizeof data;i++) data[i]=(uint8_t)random32();
    for(t=0;t<400;t++) {
        chip=t&1?RTL_8156B:RTL_8159;
        for(i=0;i<4;i++) f[i]=(struct rtl_frame){data,14+random32()%(RTL_MAX_FRAME-13)};
        if(t==0) f[0].len=RTL_MAX_FRAME;
        assert(rtl_tx_encode(chip,f,4,tx,sizeof tx,&n)==0);
        assert(rtl_tx_decode(chip,tx,n,v,4,&k)==0 && k==4);
        for(i=0;i<4;i++) {
            assert(v[i].len==(f[i].len<60?60:f[i].len));
            assert(!memcmp(v[i].data,data,f[i].len));
            for(j=f[i].len;j<v[i].len;j++) assert(!v[i].data[j]);
            bridge[i]=(struct rtl_frame){v[i].data,v[i].len};
        }
        assert(rtl_rx_fixture_encode(chip,bridge,4,rx,sizeof rx,&r)==0);
        assert(rtl_rx_decode(chip,rx,r,v,4,&k)==0 && k==4);
        for(i=0;i<4;i++) assert(v[i].len==bridge[i].len && !memcmp(v[i].data,bridge[i].data,v[i].len));
        assert(rtl_tx_encode(chip,f,4,tx,n-1,&r)==RTL_ENOSPC);
    }
}
static void bounded_random(void)
{
    uint8_t buf[1024];struct rtl_frame_view v[32];
    struct rtl_usb_interface usb;struct rtl_usb_device dev;
    unsigned t;size_t i,n,k;int e;
    for(t=0;t<20000;t++) {
        n=random32()%sizeof buf;
        for(i=0;i<n;i++) buf[i]=(uint8_t)random32();
        e=rtl_rx_decode(t&1?RTL_8156B:RTL_8159,buf,n,v,32,&k);
        if(e==0) for(i=0;i<k;i++) {
            assert(v[i].data>=buf && v[i].data+v[i].len<=buf+n);
            assert(v[i].len>=14 && v[i].len<=RTL_MAX_FRAME);
        }
        (void)rtl_tx_decode(t&1?RTL_8156B:RTL_8159,buf,n,v,32,&k);
        (void)rtl_parse_device(buf,n,&dev);
        (void)rtl_parse_config(buf,n,0,0,&usb);
    }
}
static void vendor_helpers(void)
{
    struct rtl_write w;
    uint8_t h[RTL_8159_HEADER],buf[256],frame[60]={0};
    static const uint8_t byte_cr[8]={0x40,5,0x10,0xe8,0x88,0x01,4,0};
    static const uint8_t word[8]={0x40,5,0x58,0xe8,0xcc,0x01,4,0};
    static const uint8_t dword[8]={0x40,5,0x0c,0xd4,0xff,0x00,4,0};
    static const uint8_t tx[16]={60,0,0,0xc0,0,0,0,0,0xc0,3,0,0,0,0,0,0xa8};
    struct rtl_frame f[3]={{frame,60},{frame,60},{frame,60}};
    struct rtl_frame_view v[3];
    size_t written=0,count=9;
    /* 0xe813 is byte 3 of its dword: enable 0x88, value in the top byte. */
    assert(rtl_write_request(&w,RTL_MCU_PLA,0xe813,1,0x0c)==0);
    assert(!memcmp(w.setup,byte_cr,8) && w.data[3]==0x0c && !w.data[0]);
    assert(rtl_write_request(&w,RTL_MCU_PLA,0xe85a,2,0x0208)==0);
    assert(!memcmp(w.setup,word,8) && w.data[2]==0x08 && w.data[3]==0x02);
    assert(rtl_write_request(&w,RTL_MCU_USB,0xd40c,4,0x00600400)==0);
    assert(!memcmp(w.setup,dword,8) && w.data[1]==4 && w.data[2]==0x60);
    assert(rtl_write_request(&w,RTL_MCU_USB,0xd40d,2,0)==RTL_EINVAL);
    assert(rtl_write_request(&w,RTL_MCU_USB,0xd40e,4,0)==RTL_EINVAL);
    assert(rtl_write_request(&w,RTL_MCU_USB,0xd40c,1,0x100)==RTL_EINVAL);
    assert(rtl_write_request(&w,0x0200,0xd40c,4,0)==RTL_EINVAL);
    assert(rtl_8159_tx_header(h,60)==0 && !memcmp(h,tx,16));
    assert(rtl_8159_tx_header(h,59)==RTL_EINVAL);
    assert(rtl_rx_fixture_encode(RTL_8159,f,3,buf,sizeof buf,&written)==0);
    assert(rtl_8159_rx_scan(buf,written,v,3,&count)==0 && count==3 && v[2].len==60 && v[0].data==buf+16);
    /* A bad second descriptor keeps the first frame; all-or-nothing decode does not. */
    put32(buf+80,0);
    assert(rtl_8159_rx_scan(buf,written,v,3,&count)==RTL_EFORMAT && count==1);
    assert(rtl_rx_decode(RTL_8159,buf,written,v,3,&count)!=0 && !count);
    assert(rtl_8159_rx_scan(buf,70,v,3,&count)==RTL_ETRUNC && !count);
    assert(rtl_8159_rx_scan(buf,0,v,3,&count)==0 && !count);
    put32(buf+80,64u<<17);
    assert(rtl_8159_rx_scan(buf,written,v,1,&count)==RTL_ENOSPC && count==1);
}
int main(void)
{
    struct rtl_format f;
    assert(rtl_get_format((enum rtl_chip)0,&f)==RTL_EUNSUPPORTED);
    codec(RTL_8156B);codec(RTL_8159);crc_oracle();descriptors();vendor_helpers();valid_random();bounded_random();
    puts("rtl codec: synthetic fixtures, descriptor validation, 20000 bounded random inputs OK");
    return 0;
}
