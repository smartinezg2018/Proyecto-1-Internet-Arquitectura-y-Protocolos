/**
 * OperatorDashboard.java -- Panel de control para operadores IoT
 *
 * Compile:  javac OperatorDashboard.java
 * Run:      java OperatorDashboard localhost
 *           java OperatorDashboard apidominio.proyecto1-iot-eafit.org 8080
 *
 * No requiere dependencias externas -- solo JDK 11+
 */

import javax.swing.*;
import javax.swing.table.DefaultTableModel;
import javax.swing.table.DefaultTableCellRenderer;
import java.awt.*;
import java.io.*;
import java.net.*;
import java.time.LocalTime;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.List;

public class OperatorDashboard {

    // -- Configuracion -------------------------------------------------------
    static final String DEFAULT_HOST = "apidominio.proyecto1-iot-eafit.org";
    static final int    DEFAULT_PORT = 8080;
    static final int    POLL_MS      = 2000;
    static final int    RECONECT_MS  = 5000;

    // Paleta oscura (igual que el dashboard Python)
    static final Color BG_DARK    = new Color(0x1e1e2e);
    static final Color BG_SURFACE = new Color(0x313244);
    static final Color BG_HEADER  = new Color(0x45475a);
    static final Color FG_TEXT    = new Color(0xcdd6f4);
    static final Color FG_GREEN   = new Color(0xa6e3a1);
    static final Color FG_RED     = new Color(0xf38ba8);

    // -- Estado --------------------------------------------------------------
    final String   host;
    final int      port;
    volatile boolean running = true;

    // -- Componentes Swing ---------------------------------------------------
    JFrame            frame;
    JLabel            statusLabel;
    DefaultTableModel tableModel;
    JTextArea         alertsArea;

    public OperatorDashboard(String host, int port) {
        this.host = host;
        this.port = port;
    }

    // -- DNS: nunca IP hardcodeada -------------------------------------------
    InetSocketAddress resolverHost() {
        try {
            InetAddress addr = InetAddress.getByName(host);
            return new InetSocketAddress(addr, port);
        } catch (UnknownHostException e) {
            setStatus("DNS no resuelto para " + host, FG_RED);
            return null;
        }
    }

    // -- Leer del socket caracter a caracter hasta \n -----------------------
    //
    // Por que no usar BufferedReader.readLine()?
    //
    // El servidor en C mantiene la conexion abierta despues de enviar la
    // respuesta. BufferedReader.readLine() bloquea hasta recibir \n O hasta
    // que la conexion se cierre. Como el servidor no cierra, readLine() se
    // queda bloqueado para siempre aunque ya recibio todos los datos.
    //
    // Con read() caracter a caracter y setSoTimeout(5000) en el socket,
    // la lectura termina en cuanto llega el \n que el servidor agrega a
    // cada respuesta, sin necesidad de cerrar la conexion.
    //
    String leerLinea(InputStream is) throws IOException {
        StringBuilder sb = new StringBuilder();
        int c;
        while ((c = is.read()) != -1) {
            if (c == '\n') break;
            if (c != '\r') sb.append((char) c);
        }
        return sb.toString();
    }

    // -- Construccion de la ventana -----------------------------------------
    void buildUI() {
        frame = new JFrame("EAFIT IoT Monitor");
        frame.setDefaultCloseOperation(JFrame.DO_NOTHING_ON_CLOSE);
        frame.addWindowListener(new java.awt.event.WindowAdapter() {
            public void windowClosing(java.awt.event.WindowEvent e) {
                running = false;
                frame.dispose();
            }
        });
        frame.setSize(820, 560);
        frame.setLocationRelativeTo(null);
        frame.getContentPane().setBackground(BG_DARK);
        frame.setLayout(new BorderLayout(0, 8));

        // -- Titulo + estado -------------------------------------------------
        JPanel topPanel = new JPanel();
        topPanel.setLayout(new BoxLayout(topPanel, BoxLayout.Y_AXIS));
        topPanel.setBackground(BG_DARK);
        topPanel.setBorder(BorderFactory.createEmptyBorder(14, 14, 4, 14));

        JLabel title = new JLabel("Sistema de Monitoreo IoT - EAFIT");
        title.setFont(new Font("SansSerif", Font.BOLD, 16));
        title.setForeground(FG_TEXT);
        title.setAlignmentX(Component.CENTER_ALIGNMENT);
        topPanel.add(title);
        topPanel.add(Box.createVerticalStrut(4));

        statusLabel = new JLabel("Conectando...");
        statusLabel.setFont(new Font("SansSerif", Font.PLAIN, 11));
        statusLabel.setForeground(FG_GREEN);
        statusLabel.setAlignmentX(Component.CENTER_ALIGNMENT);
        topPanel.add(statusLabel);

        frame.add(topPanel, BorderLayout.NORTH);

        // -- Tabla de sensores -----------------------------------------------
        String[] cols = {"ID", "Tipo", "Valor", "Ultima lectura"};
        tableModel = new DefaultTableModel(cols, 0) {
            public boolean isCellEditable(int r, int c) { return false; }
        };

        JTable table = new JTable(tableModel);
        table.setBackground(BG_SURFACE);
        table.setForeground(FG_TEXT);
        table.setSelectionBackground(BG_HEADER);
        table.setSelectionForeground(FG_TEXT);
        table.setGridColor(BG_HEADER);
        table.setFont(new Font("SansSerif", Font.PLAIN, 13));
        table.setRowHeight(28);
        table.setShowHorizontalLines(true);
        table.setShowVerticalLines(false);
        table.getTableHeader().setBackground(BG_HEADER);
        table.getTableHeader().setForeground(FG_TEXT);
        table.getTableHeader().setFont(new Font("SansSerif", Font.BOLD, 13));

        // Renderer: centra y colorea rojo filas con "Falla"
        table.setDefaultRenderer(Object.class, new DefaultTableCellRenderer() {
            public Component getTableCellRendererComponent(
                    JTable t, Object value, boolean sel, boolean focus, int row, int col) {
                super.getTableCellRendererComponent(t, value, sel, focus, row, col);
                setHorizontalAlignment(SwingConstants.CENTER);
                String valor = (String) t.getModel().getValueAt(row, 2);
                if (valor != null && valor.contains("Falla")) {
                    setBackground(new Color(0x45, 0x1d, 0x1d));
                    setForeground(FG_RED);
                } else {
                    setBackground(sel ? BG_HEADER : BG_SURFACE);
                    setForeground(FG_TEXT);
                }
                return this;
            }
        });

        JScrollPane tableScroll = new JScrollPane(table);
        tableScroll.getViewport().setBackground(BG_SURFACE);
        tableScroll.setBorder(BorderFactory.createEmptyBorder(0, 12, 0, 12));
        frame.add(tableScroll, BorderLayout.CENTER);

        // -- Panel de alertas ------------------------------------------------
        JPanel alertPanel = new JPanel(new BorderLayout(0, 4));
        alertPanel.setBackground(BG_DARK);
        alertPanel.setBorder(BorderFactory.createEmptyBorder(0, 14, 10, 14));

        JLabel alertTitle = new JLabel("Alertas en tiempo real");
        alertTitle.setFont(new Font("SansSerif", Font.BOLD, 12));
        alertTitle.setForeground(FG_RED);
        alertPanel.add(alertTitle, BorderLayout.NORTH);

        alertsArea = new JTextArea(6, 0);
        alertsArea.setEditable(false);
        alertsArea.setBackground(BG_DARK);
        alertsArea.setForeground(FG_RED);
        alertsArea.setFont(new Font("Monospaced", Font.PLAIN, 11));
        alertsArea.setBorder(BorderFactory.createEmptyBorder(4, 4, 4, 4));

        JScrollPane alertScroll = new JScrollPane(alertsArea);
        alertScroll.getViewport().setBackground(BG_DARK);
        alertScroll.setBorder(BorderFactory.createLineBorder(BG_HEADER));
        alertPanel.add(alertScroll, BorderLayout.CENTER);

        frame.add(alertPanel, BorderLayout.SOUTH);
        frame.setVisible(true);
    }

    // -- Helpers thread-safe ------------------------------------------------
    void setStatus(String msg, Color color) {
        SwingUtilities.invokeLater(() -> {
            statusLabel.setText(msg);
            statusLabel.setForeground(color);
        });
    }

    void actualizarTabla(String data) {
        List<String[]> filas = new ArrayList<>();
        for (String entry : data.split(";")) {
            entry = entry.trim();
            if (entry.isEmpty() || entry.equals("EMPTY")) continue;
            String[] partes = entry.split(":", 4);  // limite 4: el timestamp HH:mm:ss contiene ":" y sin limite da 6 partes en vez de 4
            if (partes.length == 4) {
                filas.add(partes);
            } else if (partes.length == 2) {
                filas.add(new String[]{partes[0], "?", partes[1], "--"});
            }
        }
        SwingUtilities.invokeLater(() -> {
            tableModel.setRowCount(0);
            for (String[] fila : filas) tableModel.addRow(fila);
        });
    }

    void mostrarAlerta(String texto) {
        String ts  = LocalTime.now().format(DateTimeFormatter.ofPattern("HH:mm:ss"));
        String msg = "[" + ts + "] !  " + texto + "\n";
        SwingUtilities.invokeLater(() -> {
            alertsArea.append(msg);
            alertsArea.setCaretPosition(alertsArea.getDocument().getLength());
        });
    }

    // -- Hilo 1: GET_STATE cada 2 segundos ----------------------------------
    void startPolling() {
        Thread t = new Thread(() -> {
            while (running) {
                try {
                    InetSocketAddress addr = resolverHost();
                    if (addr == null) { Thread.sleep(RECONECT_MS); continue; }

                    try (Socket sock = new Socket()) {
                        sock.connect(addr, 5000);
                        sock.setSoTimeout(5000);

                        OutputStream out = sock.getOutputStream();
                        InputStream  in  = sock.getInputStream();

                        out.write("GET_STATE\n".getBytes("UTF-8"));
                        out.flush();

                        String data = leerLinea(in);
                        if (!data.isEmpty()) {
                            String ts = LocalTime.now()
                                .format(DateTimeFormatter.ofPattern("HH:mm:ss"));
                            setStatus("Conectado  " + addr.getHostString()
                                + ":" + port + "  " + ts, FG_GREEN);
                            actualizarTabla(data);
                        }
                    }

                } catch (SocketTimeoutException e) {
                    setStatus("Timeout - servidor no responde", FG_RED);
                } catch (ConnectException e) {
                    setStatus("Sin conexion al servidor", FG_RED);
                } catch (Exception e) {
                    setStatus("Error: " + e.getMessage(), FG_RED);
                }

                try { Thread.sleep(POLL_MS); } catch (InterruptedException ignored) {}
            }
        }, "hilo-polling");
        t.setDaemon(true);
        t.start();
    }

    // -- Hilo 2: SUBSCRIBE - alertas push -----------------------------------
    void startAlertListener() {
        Thread t = new Thread(() -> {
            while (running) {
                InetSocketAddress addr = resolverHost();
                if (addr == null) {
                    try { Thread.sleep(RECONECT_MS); } catch (InterruptedException ignored) {}
                    continue;
                }

                try (Socket sock = new Socket()) {
                    sock.connect(addr, 5000);

                    OutputStream out = sock.getOutputStream();
                    InputStream  in  = sock.getInputStream();

                    out.write("SUBSCRIBE\n".getBytes("UTF-8"));
                    out.flush();

                    // Leer ACK con timeout corto
                    sock.setSoTimeout(5000);
                    String ack = leerLinea(in);
                    if (!ack.contains("OK")) {
                        Thread.sleep(RECONECT_MS);
                        continue;
                    }

                    // Esperar alertas push sin timeout (conexion persistente)
                    sock.setSoTimeout(0);
                    while (running) {
                        String linea = leerLinea(in);
                        if (linea.isEmpty()) break;  // servidor cerro la conexion
                        if (linea.startsWith("ALERT|")) {
                            mostrarAlerta(linea.substring(6));
                        }
                    }

                } catch (Exception e) {
                    // reconectar silenciosamente
                }

                try { Thread.sleep(RECONECT_MS); } catch (InterruptedException ignored) {}
            }
        }, "hilo-alertas");
        t.setDaemon(true);
        t.start();
    }

    // -- Arrancar -----------------------------------------------------------
    public void start() {
        SwingUtilities.invokeLater(() -> {
            buildUI();
            startPolling();
            startAlertListener();
        });
    }

    // -- main ---------------------------------------------------------------
    public static void main(String[] args) {
        String host = DEFAULT_HOST;
        int    port = DEFAULT_PORT;

        if (args.length >= 1) host = args[0];
        if (args.length >= 2) {
            try { port = Integer.parseInt(args[1]); }
            catch (NumberFormatException e) {
                System.err.println("Puerto invalido: " + args[1]);
            }
        }

        System.out.println("=== EAFIT IoT Monitor ===");
        System.out.println("Host: " + host + "  Puerto: " + port);
        new OperatorDashboard(host, port).start();
    }
}