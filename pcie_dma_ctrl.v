module pcie_dma_ctrl(
    input wire             clk                ,
    input wire             pix_clk_out        ,
    input wire             rstn               ,
    input wire             axis_master_tvalid /* synthesis PAP_MARK_DEBUG="1" */,    
    output wire            axis_master_tready /* synthesis PAP_MARK_DEBUG="1" */,    
    input wire    [127:0]  axis_master_tdata  /* synthesis PAP_MARK_DEBUG="1" */,    
    input wire    [3:0]    axis_master_tkeep  /* synthesis PAP_MARK_DEBUG="1" */,    
    input wire             axis_master_tlast  /* synthesis PAP_MARK_DEBUG="1" */,    
    input wire    [7:0]    axis_master_tuser  /* synthesis PAP_MARK_DEBUG="1" */,     
    
    input  [7 : 0]            ep_bus_num      ,
    input  [4 : 0]            ep_dev_num      ,

    input                     AXIS_S_TREADY   /* synthesis PAP_MARK_DEBUG="1" */,
    output                    AXIS_S_TVALID   /* synthesis PAP_MARK_DEBUG="1" */,
    output [127:0]            AXIS_S_TDATA    /* synthesis PAP_MARK_DEBUG="1" */,
    output                    AXIS_S_TLAST    /* synthesis PAP_MARK_DEBUG="1" */,
    output                    AXIS_S_TUSER    /* synthesis PAP_MARK_DEBUG="1" */,
//HDMI FIFO DATA IN
    input  [15: 0]           hdmi_data_in        /* synthesis PAP_MARK_DEBUG="1" */,
   //input  [8  : 0]           rd_water_level     /* synthesis PAP_MARK_DEBUG="1" */,
   //output reg                pcie_rd_en         /* synthesis PAP_MARK_DEBUG="1" */,
   //output reg                fram_start         /* synthesis PAP_MARK_DEBUG="1" */,
    input                     vs_in                ,
    input                     de_in                ,
    input                     pcie_dma_enable      ,
//PCIe ctrl
    output reg [7 : 0]        video_enhance_lightdown_num    /* synthesis PAP_MARK_DEBUG="1" */,
    output reg                video_enhance_lightdown_sw     /* synthesis PAP_MARK_DEBUG="1" */,        
    output reg [7 : 0]        video_enhance_darkup_num       /* synthesis PAP_MARK_DEBUG="1" */,
    output reg                video_enhance_darkup_sw        /* synthesis PAP_MARK_DEBUG="1" */ 
      
   );                                         
/*****************************wire******************************/
wire [12 : 0]    rd_water_level /* synthesis PAP_MARK_DEBUG="1" */;
wire [127:0]    pcie_dma_data  /* synthesis PAP_MARK_DEBUG="1" */;
/*****************************reg******************************/
localparam MRD_32    = 8'h00;
localparam MWR_32    = 8'h40;

localparam DMA_CMD_ARM_ADDR                 = 12'h100;
localparam DMA_CMD_START_ADDR               = 12'h104;
localparam DMA_CMD_LEN_ADDR                 = 12'h108;
localparam DMA_CMD_L_ADDR                   = 12'h110;
localparam DMA_CMD_ADDR_ECHO0_ADDR          = 12'h114;
localparam DMA_CMD_ADDR_ECHO1_ADDR          = 12'h118;
localparam DMA_CMD_ADDR_ECHO2_ADDR          = 12'h11c;
localparam DMA_CMD_ADDR_ECHO3_ADDR          = 12'h120;
localparam DMA_CMD_CLEAR_ADDR               = 12'h130;
localparam DMA_CMD_VIDEO_HIGHTLIGHT_SUB     = 12'h140;
localparam DMA_CMD_VIDEO_LOWLIGHT_ADD       = 12'h150;
localparam DMA_CMD_VIDEO_ENHANCE_CLEAR      = 12'h160;
localparam DMA_CMD_STATUS_ADDR              = 12'h170;

localparam DMA_ARM_MAGIC                    = 32'hA55A5AA5;
localparam [31:0] DMA_DEFAULT_BYTES         = 32'd4147264;
localparam [31:0] DMA_MAX_BYTES             = 32'd4194304;
localparam [31:0] DMA_FIXED_TEST_BYTES      = 32'd64;
parameter  ALLOC_ADDR_1   = 4'd1;
parameter  ALLOC_ADDR_2   = 4'd2;
parameter  ALLOC_ADDR_3   = 4'd3;
parameter  ALLOC_ADDR_4   = 4'd4;


parameter  MWR_IDLE       = 2'd0;
parameter  MWR_TLP_HEADER = 2'd1;
parameter  MWR_TLP_DATA   = 2'd2;
parameter  CPLD_IDLE      = 2'd0;
parameter  CPLD_HEADER    = 2'd1;
parameter  CPLD_DATA      = 2'd2;
parameter  TLP_LENGTH     = 10'd16;
parameter [15:0] DMA_TRAN_TIMES = 16'd64801;// 64801*64=1920*1080*2+64
reg [1:0]   mwr_state /* synthesis PAP_MARK_DEBUG="1" */;
reg         r_axis_s_tvalid/* synthesis PAP_MARK_DEBUG="1" */;
reg [127:0] r_axis_s_tdata;
reg         r_axis_s_tlast;
reg [10 :0] r_tlp_length_cnt/* synthesis PAP_MARK_DEBUG="1" */;

reg [127:0] axis_master_tdata_d0    /* synthesis PAP_MARK_DEBUG="1" */;
reg         axis_master_tvalid_d0   /* synthesis PAP_MARK_DEBUG="1" */;
reg         axis_master_tvalid_d1   /* synthesis PAP_MARK_DEBUG="1" */;
reg [2 : 0] tlp_fmt             /* synthesis PAP_MARK_DEBUG="1" */;
reg [4 : 0] tlp_type            /* synthesis PAP_MARK_DEBUG="1" */;
reg         mwr32               /* synthesis PAP_MARK_DEBUG="1" */;
reg [31: 0] mwr_addr            /* synthesis PAP_MARK_DEBUG="1" */;
reg [11: 0] cmd_reg_addr        /* synthesis PAP_MARK_DEBUG="1" */;
reg [9 : 0] tlp_lenght           /* synthesis PAP_MARK_DEBUG="1" */;
reg [15: 0] tlp_req_id          /* synthesis PAP_MARK_DEBUG="1" */;
reg [7 : 0] tlp_tag             /* synthesis PAP_MARK_DEBUG="1" */;
reg [2 : 0] tlp_tc              /* synthesis PAP_MARK_DEBUG="1" */;
reg [2 : 0] tlp_attr            /* synthesis PAP_MARK_DEBUG="1" */;
reg [6 : 0] tlp_low_addr        /* synthesis PAP_MARK_DEBUG="1" */;
reg         rc_cfg_ep_flag      /* synthesis PAP_MARK_DEBUG="1" */;
reg [31: 0] alloc_addrl         /* synthesis PAP_MARK_DEBUG="1" */;
//分配的帧缓存地址
reg [3 : 0] alloc_addr_state    /* synthesis PAP_MARK_DEBUG="1" */;
reg [1 : 0] addr_page           /* synthesis PAP_MARK_DEBUG="1" */;
reg [31: 0] dma_addr0           /* synthesis PAP_MARK_DEBUG="1" */;
reg [31: 0] dma_addr1           /* synthesis PAP_MARK_DEBUG="1" */;
reg [31: 0] dma_addr2           /* synthesis PAP_MARK_DEBUG="1" */;
reg [31: 0] dma_addr3           /* synthesis PAP_MARK_DEBUG="1" */;
reg         dma_addr_cfg_flag   /* synthesis PAP_MARK_DEBUG="1" */;
reg         dma_stop_flag       /* synthesis PAP_MARK_DEBUG="1" */;
reg         host_arm_flag       /* synthesis PAP_MARK_DEBUG="1" */;
reg         host_start_flag     /* synthesis PAP_MARK_DEBUG="1" */;
reg [31: 0] dma_len_bytes       /* synthesis PAP_MARK_DEBUG="1" */;
reg         frame_done_flag     /* synthesis PAP_MARK_DEBUG="1" */;
reg         addr_error_flag     /* synthesis PAP_MARK_DEBUG="1" */;
reg [31: 0] dma_status          /* synthesis PAP_MARK_DEBUG="1" */;
reg [1 : 0] cpld_state          /* synthesis PAP_MARK_DEBUG="1" */;
reg         cpld_req_valid      /* synthesis PAP_MARK_DEBUG="1" */;
reg [31: 0] cpld_read_data      /* synthesis PAP_MARK_DEBUG="1" */;
reg [15: 0] cpld_req_id         /* synthesis PAP_MARK_DEBUG="1" */;
reg [7 : 0] cpld_tag            /* synthesis PAP_MARK_DEBUG="1" */;
reg [2 : 0] cpld_tc             /* synthesis PAP_MARK_DEBUG="1" */;
reg [2 : 0] cpld_attr           /* synthesis PAP_MARK_DEBUG="1" */;
reg [6 : 0] cpld_low_addr       /* synthesis PAP_MARK_DEBUG="1" */;
reg [1 : 0] rc_cfg_cnt          /* synthesis PAP_MARK_DEBUG="1" */;
reg [15: 0] dma_cnt             /* synthesis PAP_MARK_DEBUG="1" */;
reg         rc_cfg_ep_flag_d0   /* synthesis PAP_MARK_DEBUG="1" */;
reg         vs_in_d0            /* synthesis PAP_MARK_DEBUG="1" */;
reg         vs_in_d1            /* synthesis PAP_MARK_DEBUG="1" */;
reg         r_pre_rd_flag        /* synthesis PAP_MARK_DEBUG="1" */;
reg         fram_start        /* synthesis PAP_MARK_DEBUG="1" */;
reg         dma_start        /* synthesis PAP_MARK_DEBUG="1" */;
reg         pcie_rd_en        /* synthesis PAP_MARK_DEBUG="1" */;
reg [7  : 0]  fram_cnt    /* synthesis PAP_MARK_DEBUG="1" */;
/*****************************assign******************************/
assign axis_master_tready = 'd1;
assign AXIS_S_TVALID = r_axis_s_tvalid;
assign AXIS_S_TDATA  = r_axis_s_tdata ;
assign AXIS_S_TLAST  = r_axis_s_tlast ;
assign AXIS_S_TUSER  = 'd0;
wire [31:0] cmd_data = {axis_master_tdata_d0[7:0], axis_master_tdata_d0[15:8],
                        axis_master_tdata_d0[23:16], axis_master_tdata_d0[31:24]};
wire [31:0] requested_tlps = (dma_len_bytes + 32'd63) >> 6;
wire [15:0] dma_tlp_limit = (requested_tlps == 32'd0) ? 16'd1 :
                             (requested_tlps > {16'd0,DMA_TRAN_TIMES}) ? DMA_TRAN_TIMES : requested_tlps[15:0];
wire        dma_addr_valid = (dma_addr0 != 32'd0) && (dma_addr1 != 32'd0) &&
                             (dma_addr2 != 32'd0) && (dma_addr3 != 32'd0) &&
                             (dma_addr0[1:0] == 2'b00) && (dma_addr1[1:0] == 2'b00) &&
                             (dma_addr2[1:0] == 2'b00) && (dma_addr3[1:0] == 2'b00);
wire        dma_len_valid  = (dma_len_bytes >= DMA_FIXED_TEST_BYTES) &&
                             (dma_len_bytes <= DMA_MAX_BYTES) &&
                             (dma_len_bytes[5:0] == 6'd0);
wire        full_frame_mode = (dma_len_bytes >= DMA_DEFAULT_BYTES);
wire        fixed_test_mode = (dma_len_bytes >= DMA_FIXED_TEST_BYTES) &&
                             (dma_len_bytes < DMA_DEFAULT_BYTES);
wire        final_tlp = (dma_cnt >= (dma_tlp_limit - 16'd1));
wire        pre_final_tlp = (dma_tlp_limit > 16'd1) && (dma_cnt == (dma_tlp_limit - 16'd2));
wire        dma_busy_status = rc_cfg_ep_flag || fram_start || dma_start || r_axis_s_tvalid;
wire        mwr_tx_active = pcie_dma_enable && fram_start && dma_start && rc_cfg_ep_flag;
wire        mwr_tlast = AXIS_S_TLAST && mwr_tx_active && (mwr_state == MWR_TLP_DATA);
wire [31:0] cpld_read_data_tx = {cpld_read_data[7:0], cpld_read_data[15:8],
                                 cpld_read_data[23:16], cpld_read_data[31:24]};
wire [31:0] cpld_header0 = {3'b010,5'b01010,1'b0,cpld_tc,1'b0,cpld_attr[2],
                            1'b0,1'b0,1'b0,1'b0,cpld_attr[1:0],2'b00,10'd1};
wire [127:0] fixed_dma_payload =
    (r_tlp_length_cnt == 11'd0) ? {32'hA5A50003,32'hA5A50002,32'hA5A50001,32'hA5A50000} :
    (r_tlp_length_cnt == 11'd1) ? {32'hA5A50007,32'hA5A50006,32'hA5A50005,32'hA5A50004} :
    (r_tlp_length_cnt == 11'd2) ? {32'hA5A5000B,32'hA5A5000A,32'hA5A50009,32'hA5A50008} :
                                  {32'hA5A5000F,32'hA5A5000E,32'hA5A5000D,32'hA5A5000C};
/************************always**********************************/
//对输入数据多打一拍
always  @(posedge clk) begin
    if(!rstn) begin
        axis_master_tdata_d0 <= 'd0;
        axis_master_tvalid_d0 <= 'd0;
        rc_cfg_ep_flag_d0 <= 'd0;
        vs_in_d0 <= 'd0;
        vs_in_d1 <= 'd0;
    end
    else begin
        axis_master_tdata_d0 <= axis_master_tdata;
        axis_master_tvalid_d0 <= axis_master_tvalid;
        axis_master_tvalid_d1 <= axis_master_tvalid_d0;
        rc_cfg_ep_flag_d0 <= rc_cfg_ep_flag;
        vs_in_d0 <= vs_in;
        vs_in_d1 <= vs_in_d0;
    end
end
//根据输入数据分析格式
always  @(posedge clk) begin
    if(!rstn) begin
        tlp_fmt  <= 'd0;
        tlp_type <= 'd0;
        mwr32    <= 'd0;
        mwr_addr <= 'd0;
        cmd_reg_addr <= 'd0;
        tlp_lenght <= 'd0;
        tlp_req_id <= 'd0;
        tlp_tag <= 'd0;
        tlp_tc <= 'd0;
        tlp_attr <= 'd0;
        tlp_low_addr <= 'd0;
    end
    else if(axis_master_tvalid_d0 && !(axis_master_tvalid_d1))begin
        tlp_fmt  <= axis_master_tdata_d0[31:29];
        tlp_type <= axis_master_tdata_d0[28:24];
        mwr_addr <= axis_master_tdata_d0[95:64];
        cmd_reg_addr <= axis_master_tdata_d0[75:64];//cmd偏移地址
        tlp_lenght <= axis_master_tdata_d0[9:0];//TLP数据包长度
        tlp_req_id <= axis_master_tdata_d0[63:48];
        tlp_tag <= axis_master_tdata_d0[47:40];
        tlp_tc <= axis_master_tdata_d0[22:20];
        tlp_attr <= {axis_master_tdata_d0[18],axis_master_tdata_d0[13:12]};
        tlp_low_addr <= {axis_master_tdata_d0[70:66],2'b0};
    end
    else begin
        tlp_fmt  <= 'd0;
        tlp_type <= 'd0;
        mwr_addr <= 'd0;
        cmd_reg_addr <= 'd0;
        tlp_lenght <= 'd0;
        tlp_req_id <= 'd0;
        tlp_tag <= 'd0;
        tlp_tc <= 'd0;
        tlp_attr <= 'd0;
        tlp_low_addr <= 'd0;
    end
end

always  @(posedge clk) begin// Host MWr command decoder
    if(!rstn || !pcie_dma_enable || dma_stop_flag) begin
        alloc_addr_state  <= ALLOC_ADDR_1;
        dma_addr0         <= 'd0;
        dma_addr1         <= 'd0;
        dma_addr2         <= 'd0;
        dma_addr3         <= 'd0;
        dma_stop_flag     <= 'd0;
        host_arm_flag     <= 'd0;
        host_start_flag   <= 'd0;
        dma_len_bytes     <= 32'd0;
        frame_done_flag   <= 'd0;
        addr_error_flag   <= 'd0;
        video_enhance_lightdown_num <= 'd0;
        video_enhance_lightdown_sw  <= 'd0;
        video_enhance_darkup_num    <= 'd0;
        video_enhance_darkup_sw     <= 'd0;
    end
    else if(({tlp_fmt,tlp_type} == MWR_32 )&& (tlp_lenght == 1) && (cmd_reg_addr == DMA_CMD_ARM_ADDR)) begin
        if(cmd_data == DMA_ARM_MAGIC) begin
            host_arm_flag     <= 1'b1;
            host_start_flag   <= 1'b0;
            frame_done_flag   <= 1'b0;
            addr_error_flag   <= 1'b0;
            alloc_addr_state  <= ALLOC_ADDR_1;
        end
        else begin
            host_arm_flag     <= 1'b0;
            host_start_flag   <= 1'b0;
        end
    end
    else if(({tlp_fmt,tlp_type} == MWR_32 )&& (tlp_lenght == 1) && (cmd_reg_addr == DMA_CMD_LEN_ADDR) && host_arm_flag) begin
        dma_len_bytes   <= cmd_data;
        frame_done_flag <= 1'b0;
        addr_error_flag <= 1'b0;
    end
    else if(({tlp_fmt,tlp_type} == MWR_32 )&& (tlp_lenght == 1) && (cmd_reg_addr == DMA_CMD_L_ADDR) && (host_arm_flag || (cmd_data == 32'd0))) begin
        case(alloc_addr_state)
            ALLOC_ADDR_1:
            begin
                dma_addr0  <= cmd_data;
                alloc_addr_state <= ALLOC_ADDR_2;
            end
            ALLOC_ADDR_2:
            begin
                dma_addr1  <= cmd_data;
                alloc_addr_state <= ALLOC_ADDR_3;
            end
            ALLOC_ADDR_3:
            begin
                dma_addr2  <= cmd_data;
                alloc_addr_state <= ALLOC_ADDR_4;
            end
            ALLOC_ADDR_4:
            begin
                dma_addr3  <= cmd_data;
                alloc_addr_state <= ALLOC_ADDR_1;
            end
        endcase
    end
    else if(({tlp_fmt,tlp_type} == MWR_32 )&& (tlp_lenght == 1) && (cmd_reg_addr == DMA_CMD_CLEAR_ADDR)) begin
        dma_stop_flag <= 1'b1;
    end
    else if(({tlp_fmt,tlp_type} == MWR_32 )&& (tlp_lenght == 1) && (cmd_reg_addr == DMA_CMD_START_ADDR) && host_arm_flag && cmd_data[0]) begin
        if(dma_addr_valid && dma_len_valid) begin
            host_start_flag <= 1'b1;
            frame_done_flag <= 1'b0;
            addr_error_flag <= 1'b0;
        end
        else begin
            host_start_flag <= 1'b0;
            addr_error_flag <= 1'b1;
        end
    end
    else if(mwr_tlast && final_tlp) begin
        host_start_flag <= 1'b0;
        host_arm_flag   <= 1'b0;
        frame_done_flag <= 1'b1;
    end
    else if(({tlp_fmt,tlp_type} == MWR_32 )&& (tlp_lenght == 1) && (cmd_reg_addr == DMA_CMD_VIDEO_LOWLIGHT_ADD)) begin//暗光增强命令
        video_enhance_darkup_num <= axis_master_tdata_d0[31:24];
        video_enhance_darkup_sw <= 1'b1;
    end
    else if(({tlp_fmt,tlp_type} == MWR_32 )&& (tlp_lenght == 1) && (cmd_reg_addr == DMA_CMD_VIDEO_HIGHTLIGHT_SUB)) begin//高光压制命令
        video_enhance_lightdown_num <= axis_master_tdata_d0[31:24];
        video_enhance_lightdown_sw <= 1'b1;
    end
    else if(({tlp_fmt,tlp_type} == MWR_32 )&& (tlp_lenght == 1) && (cmd_reg_addr == DMA_CMD_VIDEO_ENHANCE_CLEAR)) begin//停止视频增强
        video_enhance_lightdown_sw <= 1'b0;
        video_enhance_darkup_sw    <= 1'b0;
        video_enhance_lightdown_num <= 'd0;
        video_enhance_darkup_num   <= 'd0;
    end
end

always  @(posedge clk) begin// DMA transfer state flags
    if(!rstn  || !pcie_dma_enable || dma_stop_flag) begin
        rc_cfg_ep_flag  <= 'd0;
        dma_cnt         <= 'd0;
        fram_start      <= 'd0;
        dma_start       <= 'd0;
    end
    else begin
        if(host_start_flag && dma_addr_valid && dma_len_valid) begin
            rc_cfg_ep_flag <= 'd1;
            if(fixed_test_mode) begin
                fram_start <= 'd1;
                dma_start  <= 'd1;
            end
        end
        if(!fixed_test_mode && !vs_in_d0 && vs_in_d1 && rc_cfg_ep_flag) begin
            fram_start <= 'd1;
            dma_start  <= 'd0;
        end
        if(!fixed_test_mode && fram_start && (vs_in_d0 && !vs_in_d1)) begin
            fram_start <= 'd0;
        end
        if(mwr_tlast) begin
            dma_start <= 'd0;
            if(final_tlp) begin
                dma_cnt        <= 'd0;
                rc_cfg_ep_flag <= 'd0;
                fram_start     <= 'd0;
            end
            else begin
                dma_cnt <= dma_cnt + 'd1;
            end
        end
        else if(!fixed_test_mode && rc_cfg_ep_flag && (((dma_cnt == 0) && (rd_water_level >= TLP_LENGTH/4 + 1)) ||
                ((dma_cnt != 0) && (rd_water_level >= TLP_LENGTH/4)) ||
                ((rd_water_level >= TLP_LENGTH/4 - 1) && pre_final_tlp) ||
                (full_frame_mode && final_tlp))) begin
            dma_start <= 'd1;
        end
    end
end
// Minimal MRd/CplD register readback for ADDR_ECHO and STATUS.
always @(posedge clk) begin
    if(!rstn || !pcie_dma_enable || dma_stop_flag) begin
        cpld_req_valid <= 1'b0;
        cpld_read_data <= 32'd0;
        cpld_req_id    <= 16'd0;
        cpld_tag       <= 8'd0;
        cpld_tc        <= 3'd0;
        cpld_attr      <= 3'd0;
        cpld_low_addr  <= 7'd0;
    end
    else if(({tlp_fmt,tlp_type} == MRD_32) && (tlp_lenght == 1) && !cpld_req_valid) begin
        cpld_req_valid <= 1'b1;
        cpld_req_id    <= tlp_req_id;
        cpld_tag       <= tlp_tag;
        cpld_tc        <= tlp_tc;
        cpld_attr      <= tlp_attr;
        cpld_low_addr  <= tlp_low_addr;
        case(cmd_reg_addr)
            DMA_CMD_ADDR_ECHO0_ADDR: cpld_read_data <= dma_addr0;
            DMA_CMD_ADDR_ECHO1_ADDR: cpld_read_data <= dma_addr1;
            DMA_CMD_ADDR_ECHO2_ADDR: cpld_read_data <= dma_addr2;
            DMA_CMD_ADDR_ECHO3_ADDR: cpld_read_data <= dma_addr3;
            DMA_CMD_STATUS_ADDR    : cpld_read_data <= dma_status;
            default                : cpld_read_data <= 32'd0;
        endcase
    end
    else if((cpld_state == CPLD_DATA) && AXIS_S_TREADY) begin
        cpld_req_valid <= 1'b0;
    end
end

always @(posedge clk) begin
    if(!rstn || !pcie_dma_enable || dma_stop_flag) begin
        cpld_state <= CPLD_IDLE;
    end
    else begin
        case(cpld_state)
            CPLD_IDLE:
            begin
                if(cpld_req_valid && !mwr_tx_active && AXIS_S_TREADY)
                    cpld_state <= CPLD_HEADER;
            end
            CPLD_HEADER:
            begin
                if(AXIS_S_TREADY)
                    cpld_state <= CPLD_DATA;
            end
            CPLD_DATA:
            begin
                if(AXIS_S_TREADY)
                    cpld_state <= CPLD_IDLE;
            end
            default:
            begin
                cpld_state <= CPLD_IDLE;
            end
        endcase
    end
end

//控制AXIS发送TLP包
always @(posedge clk) begin
    if(!rstn  || !pcie_dma_enable || dma_stop_flag) begin
        r_axis_s_tvalid  <= 'd0;
        r_axis_s_tdata   <= 'd0;
        r_axis_s_tlast   <= 'd0;
        r_tlp_length_cnt <= 'd0;
        r_pre_rd_flag    <= 'd0;
        pcie_rd_en       <= 'd0;
        alloc_addrl      <= 'd0;
        addr_page        <= 'd0;
        fram_cnt         <= 'd1;
    end
    else if (!fixed_test_mode && (vs_in_d0 && !vs_in_d1)) begin//vs上升沿复位
        r_axis_s_tvalid  <= 'd0;
        r_axis_s_tdata   <= 'd0;
        r_axis_s_tlast   <= 'd0;
        r_tlp_length_cnt <= 'd0;
        r_pre_rd_flag    <= 'd0;
        pcie_rd_en       <= 'd0;
        addr_page        <= addr_page + 'd1;
        if(addr_page == 0) begin
            alloc_addrl      <= dma_addr0;
        end
        else if(addr_page == 1) begin
            alloc_addrl      <= dma_addr1;
        end
        else if(addr_page == 2) begin
            alloc_addrl      <= dma_addr2;
        end
        else if(addr_page == 3) begin
            alloc_addrl      <= dma_addr3;
        end
    end
    else if (host_start_flag && fixed_test_mode && !rc_cfg_ep_flag) begin
        r_axis_s_tvalid  <= 'd0;
        r_axis_s_tlast   <= 'd0;
        r_tlp_length_cnt <= 'd0;
        r_pre_rd_flag    <= 'd0;
        pcie_rd_en       <= 'd0;
        alloc_addrl      <= dma_addr0;
        addr_page        <= 'd0;
    end
    else if (pcie_dma_enable && (cpld_state != CPLD_IDLE)) begin
        case(cpld_state)
            CPLD_HEADER:
            begin
                r_axis_s_tvalid <= 1'b1;
                r_axis_s_tlast  <= 1'b0;
                r_axis_s_tdata  <= {32'd0,cpld_req_id,cpld_tag,1'b0,cpld_low_addr,
                                    {ep_bus_num,ep_dev_num,3'b0},3'b000,1'b0,12'd4,cpld_header0};
                pcie_rd_en      <= 1'b0;
            end
            CPLD_DATA:
            begin
                r_axis_s_tvalid <= 1'b1;
                r_axis_s_tlast  <= 1'b1;
                r_axis_s_tdata  <= {96'd0,cpld_read_data_tx};
                pcie_rd_en      <= 1'b0;
            end
            default:
            begin
                r_axis_s_tvalid <= 1'b0;
                r_axis_s_tlast  <= 1'b0;
                r_axis_s_tdata  <= 128'd0;
                pcie_rd_en      <= 1'b0;
            end
        endcase
    end
    else if (mwr_tx_active) begin //(rc_cfg_ep_flag) begin //
        case(mwr_state)
            MWR_IDLE:
            begin
                r_axis_s_tvalid <= 'd0;
                r_axis_s_tdata  <= 'd0;
                r_axis_s_tlast  <= 'd0;
                if(fixed_test_mode) begin
                    pcie_rd_en <= 'd0;
                    r_pre_rd_flag <= 'd1;
                end
                else if(r_pre_rd_flag == 0) begin
                    pcie_rd_en <= 'd1;
                    r_pre_rd_flag <= 'd1;
                end
                else begin
                    pcie_rd_en <= 'd0;
                end
            end
            MWR_TLP_HEADER:
            begin
                r_axis_s_tvalid <= 'd1;
                r_axis_s_tlast  <= 'd0;
                r_axis_s_tdata[9  :  0]  <= TLP_LENGTH;
                r_axis_s_tdata[11 : 10]  <= 'h0;
                r_axis_s_tdata[13 : 12]  <= 'h0;
                r_axis_s_tdata[14]       <= 'h0;
                r_axis_s_tdata[15]       <= 'h0;
                r_axis_s_tdata[16]       <= 'h0;
                r_axis_s_tdata[17]       <= 'h0;
                r_axis_s_tdata[18]       <= 'h0;
                r_axis_s_tdata[19]       <= 'h0;
                r_axis_s_tdata[22 : 20]  <= 'h0;
                r_axis_s_tdata[23]       <= 'h0;
                r_axis_s_tdata[28 : 24]  <= 'h0;
                r_axis_s_tdata[31 : 29]  <= 3'b010;
                r_axis_s_tdata[35 : 32]  <= 4'hf;
                r_axis_s_tdata[39 : 36]  <= 4'hf;
                r_axis_s_tdata[47 : 40]  <= 8'h01;
                r_axis_s_tdata[63 : 48]  <= {ep_bus_num,ep_dev_num,3'b0};
                r_axis_s_tdata[95 : 64]  <= alloc_addrl;
                r_axis_s_tdata[127: 96]  <= 32'd0;
                pcie_rd_en <= fixed_test_mode ? 1'b0 : 1'b1;
            end
            MWR_TLP_DATA:
            begin
                if(AXIS_S_TREADY) begin
                    if(fixed_test_mode) begin
                        r_axis_s_tdata <= fixed_dma_payload;
                        pcie_rd_en <= 'd0;
                    end
                    else if(!(full_frame_mode && final_tlp)) begin
                        r_axis_s_tdata <= {
                                            pcie_dma_data[103: 96],pcie_dma_data[111:104],pcie_dma_data[119:112],pcie_dma_data[127:120],
                                            pcie_dma_data[71 : 64],pcie_dma_data[79 : 72],pcie_dma_data[87 : 80],pcie_dma_data[95 : 88],
                                            pcie_dma_data[39 : 32],pcie_dma_data[47 : 40],pcie_dma_data[55 : 48],pcie_dma_data[63 : 56],
                                            pcie_dma_data[7  :  0],pcie_dma_data[15 :  8],pcie_dma_data[23 : 16],pcie_dma_data[31 : 24] };
                    end
                    else if(full_frame_mode && final_tlp) begin
                        r_axis_s_tdata <= {16{fram_cnt}};
                        if(fram_cnt == 8'hff) begin
                            fram_cnt <= 'd1;
                        end
                        else begin
                            fram_cnt <= fram_cnt + 'd1;
                        end
                        pcie_rd_en <= 'd0;
                    end
                    r_tlp_length_cnt <= r_tlp_length_cnt + 1'd1;
                    if(AXIS_S_TLAST) begin
                        r_tlp_length_cnt <= 'd0;
                        r_axis_s_tvalid  <= 'd0;
                        r_axis_s_tdata   <= 'd0;
                        r_axis_s_tlast   <= 'd0;
                        alloc_addrl <= alloc_addrl + TLP_LENGTH*4;
                    end
                    else if(((TLP_LENGTH <= 4) || ((TLP_LENGTH > 4) && (r_tlp_length_cnt >= TLP_LENGTH/4 - 1)) && AXIS_S_TREADY)) begin
                        r_axis_s_tlast  <= 'd1;
                        pcie_rd_en       <= 'd0;
                    end
                end
                else begin
                    pcie_rd_en <= 'd0;
                end
            end
            default:
            begin
                r_axis_s_tvalid <= 'd0;
                r_axis_s_tdata  <= 'd0;
                r_axis_s_tlast  <= 'd0;
                pcie_rd_en      <='d0 ;
            end
        endcase
    end
    else begin
        r_axis_s_tvalid <= 1'b0;
        r_axis_s_tlast  <= 1'b0;
        r_axis_s_tdata  <= 128'd0;
        pcie_rd_en      <= 1'b0;
    end
end

always @(posedge clk) begin
    if(!rstn) begin
        dma_status <= 32'd0;
    end
    else begin
        dma_status <= {24'd0,
                       rc_cfg_ep_flag,
                       pcie_dma_enable,
                       host_start_flag,
                       host_arm_flag,
                       addr_error_flag,
                       1'b0,
                       frame_done_flag,
                       dma_busy_status};
    end
end
/************************state_machine**********************************/
//控制AXIS发送TLP包
always @(posedge clk) begin
    if(!rstn || !pcie_dma_enable || (!fixed_test_mode && (vs_in_d0 && !vs_in_d1))  || dma_stop_flag) begin
        mwr_state <= 'd0;
    end
    else if(mwr_tx_active && (cpld_state == CPLD_IDLE)) begin // (rc_cfg_ep_flag) begin //
        case(mwr_state)
            MWR_IDLE:
            begin
                if(AXIS_S_TREADY) begin
                    mwr_state <= MWR_TLP_HEADER;
                end
                else begin
                    mwr_state <= mwr_state;
                end
            end 
            MWR_TLP_HEADER:
            begin
                if(AXIS_S_TREADY) begin//当Valid和Ready同时拉高时，进入下一个状态
                    mwr_state <= MWR_TLP_DATA;
                end
                else begin
                    mwr_state <= mwr_state;
                end
            end
            MWR_TLP_DATA:
            begin
                if(AXIS_S_TLAST) begin //拉高后回归IDLE
                    mwr_state <= MWR_IDLE;
                end
                else begin
                    mwr_state <= mwr_state;
                end
            end
            default:
            begin
                    mwr_state <= mwr_state;
            end
        endcase
    end
    else begin
        mwr_state <= mwr_state;
    end
end


hdmi_pcie_fifo u_hdmi_pcie_fifo (
  .wr_clk            (pix_clk_out                ),    // input
  .wr_rst            (vs_in || ~rstn  || !pcie_dma_enable || dma_stop_flag             ),    // input
  .wr_en             (de_in && fram_start       ),    // input
  .wr_data           (hdmi_data_in               ),    // input [15:0]
  .wr_full           (                           ),    // output
  .wr_water_level    (                           ),    // output [10:0]
  .almost_full       (                           ),    // output
  .rd_clk            (clk                        ),    // input
  .rd_rst            (vs_in || ~rstn  || !pcie_dma_enable || dma_stop_flag             ),    // input
  .rd_en             (pcie_rd_en                 ),    // input
  .rd_data           (pcie_dma_data              ),    // output [127:0]
  .rd_empty          (                           ),    // output
  .rd_water_level    (rd_water_level             ),    // output [10:0]
  .almost_empty      (                           )     // output
);


endmodule