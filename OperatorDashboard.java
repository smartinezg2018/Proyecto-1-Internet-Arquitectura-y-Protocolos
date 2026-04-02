/**
 * OperatorDashboard.java -- Panel de control para operadores IoT
 *
 * Compile:  javac OperatorDashboard.java
 * Run:      java OperatorDashboard localhost
 *           java OperatorDashboard apidominio.proyecto1-iot-eafit.org 8080
 *
 * Flujo:
 *   1. Se muestra pantalla de login
 *   2. Al presionar "Ingresar", se envia LOGIN|usuario|clave al servidor
 *   3. Si el servidor responde OK|<rol>, se abre el dashboard principal
 *   4. Si responde ERR|..., se muestra mensaje de error en la pantalla de login
 *
 * No requiere dependencias externas -- solo JDK 11+
 */

import javax.swing.*;
import javax.swing.table.DefaultTableModel;
import javax.swing.table.DefaultTableCellRenderer;
import java.awt.*;
import java.awt.event.*;
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

    // Paleta oscura
    static final Color BG_DARK    = new Color(0x1e1e2e);
    static final Color BG_SURFACE = new Color(0x313244);
    static final Color BG_HEADER  = new Color(0x45475a);
    static final Color FG_TEXT    = new Color(0xcdd6f4);
    static final Color FG_GREEN   = new Color(0xa6e3a1);
    static final Color FG_RED     = new Color(0xf38ba8);
    static final Color FG_YELLOW  = new Color(0xf9e2af);
    static final Color ACCENT     = new Color(0x89b4fa);

    // -- Estado --------------------------------------------------------------
    final String   host;
    final int      port;
    volatile boolean running = false;

    // Datos del usuario autenticado
    String usuarioActual = "";
    String rolActual     = "";

    // -- Componentes Swing ---------------------------------------------------
    JFrame            frame;
    JLabel            statusLabel;
    DefaultTableModel tableModel;
    JTextArea         alertsArea;

    public OperatorDashboard(String host, int port) {
        this.host = host;
        this.port = port;
    }

    // -- DNS -----------------------------------------------------------------
    InetSocketAddress resolverHost() {
        try {
            return new InetSocketAddress(InetAddress.getByName(host), port);
        } catch (UnknownHostException e) {
            return null;
        }
    }

    // -- Leer linea del socket (caracter a caracter hasta \n) ----------------
    String leerLinea(InputStream is) throws IOException {
        StringBuilder sb = new StringBuilder();
        int c;
        while ((c = is.read()) != -1) {
            if (c == '\n') break;
            if (c != '\r') sb.append((char) c);
        }
        return sb.toString();
    }

    // ========================================================================
    // PANTALLA DE LOGIN
    // ========================================================================
    void mostrarLogin() {
        JFrame loginFrame = new JFrame("IoT Monitor - Iniciar sesion");
        loginFrame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        loginFrame.setSize(400, 320);
        loginFrame.setLocationRelativeTo(null);
        loginFrame.setResizable(false);
        loginFrame.getContentPane().setBackground(BG_DARK);
        loginFrame.setLayout(new BorderLayout());

        // -- Panel central con formulario ------------------------------------
        JPanel panel = new JPanel();
        panel.setLayout(new BoxLayout(panel, BoxLayout.Y_AXIS));
        panel.setBackground(BG_DARK);
        panel.setBorder(BorderFactory.createEmptyBorder(30, 40, 20, 40));

        // Titulo
        JLabel titulo = new JLabel("Sistema de Monitoreo IoT");
        titulo.setFont(new Font("SansSerif", Font.BOLD, 16));
        titulo.setForeground(FG_TEXT);
        titulo.setAlignmentX(Component.CENTER_ALIGNMENT);
        panel.add(titulo);

        JLabel subtitulo = new JLabel("EAFIT");
        subtitulo.setFont(new Font("SansSerif", Font.PLAIN, 12));
        subtitulo.setForeground(new Color(0x6c7086));
        subtitulo.setAlignmentX(Component.CENTER_ALIGNMENT);
        panel.add(subtitulo);
        panel.add(Box.createVerticalStrut(24));

        // Campo usuario
        JLabel lblUsuario = new JLabel("Usuario");
        lblUsuario.setForeground(FG_TEXT);
        lblUsuario.setFont(new Font("SansSerif", Font.PLAIN, 12));
        lblUsuario.setAlignmentX(Component.LEFT_ALIGNMENT);
        panel.add(lblUsuario);
        panel.add(Box.createVerticalStrut(4));

        JTextField txtUsuario = new JTextField();
        txtUsuario.setMaximumSize(new Dimension(Integer.MAX_VALUE, 36));
        txtUsuario.setBackground(BG_SURFACE);
        txtUsuario.setForeground(FG_TEXT);
        txtUsuario.setCaretColor(FG_TEXT);
        txtUsuario.setFont(new Font("SansSerif", Font.PLAIN, 13));
        txtUsuario.setBorder(BorderFactory.createCompoundBorder(
            BorderFactory.createLineBorder(BG_HEADER),
            BorderFactory.createEmptyBorder(4, 8, 4, 8)));
        panel.add(txtUsuario);
        panel.add(Box.createVerticalStrut(12));

        // Campo clave
        JLabel lblClave = new JLabel("Contrasena");
        lblClave.setForeground(FG_TEXT);
        lblClave.setFont(new Font("SansSerif", Font.PLAIN, 12));
        lblClave.setAlignmentX(Component.LEFT_ALIGNMENT);
        panel.add(lblClave);
        panel.add(Box.createVerticalStrut(4));

        JPasswordField txtClave = new JPasswordField();
        txtClave.setMaximumSize(new Dimension(Integer.MAX_VALUE, 36));
        txtClave.setBackground(BG_SURFACE);
        txtClave.setForeground(FG_TEXT);
        txtClave.setCaretColor(FG_TEXT);
        txtClave.setFont(new Font("SansSerif", Font.PLAIN, 13));
        txtClave.setBorder(BorderFactory.createCompoundBorder(
            BorderFactory.createLineBorder(BG_HEADER),
            BorderFactory.createEmptyBorder(4, 8, 4, 8)));
        panel.add(txtClave);
        panel.add(Box.createVerticalStrut(8));

        // Mensaje de error (oculto inicialmente)
        JLabel lblError = new JLabel(" ");
        lblError.setForeground(FG_RED);
        lblError.setFont(new Font("SansSerif", Font.PLAIN, 11));
        lblError.setAlignmentX(Component.CENTER_ALIGNMENT);
        panel.add(lblError);
        panel.add(Box.createVerticalStrut(8));

        // Boton ingresar
        JButton btnLogin = new JButton("Ingresar");
        btnLogin.setMaximumSize(new Dimension(Integer.MAX_VALUE, 38));
        btnLogin.setBackground(ACCENT);
        btnLogin.setForeground(BG_DARK);
        btnLogin.setFont(new Font("SansSerif", Font.BOLD, 13));
        btnLogin.setFocusPainted(false);
        btnLogin.setBorderPainted(false);
        btnLogin.setCursor(Cursor.getPredefinedCursor(Cursor.HAND_CURSOR));
        panel.add(btnLogin);

        loginFrame.add(panel, BorderLayout.CENTER);
        loginFrame.setVisible(true);
        txtUsuario.requestFocus();

        // -- Logica del login ------------------------------------------------
        ActionListener doLogin = e -> {
            String usuario = txtUsuario.getText().trim();
            String clave   = new String(txtClave.getPassword());

            if (usuario.isEmpty() || clave.isEmpty()) {
                lblError.setText("Ingresa usuario y contrasena");
                return;
            }

            btnLogin.setEnabled(false);
            btnLogin.setText("Verificando...");
            lblError.setText(" ");

            // Hacer el login en un hilo aparte para no bloquear la UI
            new Thread(() -> {
                String resultado = intentarLogin(usuario, clave);
                SwingUtilities.invokeLater(() -> {
                    btnLogin.setEnabled(true);
                    btnLogin.setText("Ingresar");

                    if (resultado.startsWith("OK|")) {
                        // Login exitoso
                        usuarioActual = usuario;
                        rolActual     = resultado.substring(3);
                        loginFrame.dispose();
                        buildDashboard();
                    } else {
                        // Mostrar error según respuesta
                        if (resultado.contains("UNAVAILABLE") || resultado.contains("TIMEOUT")) {
                            lblError.setText("Servicio de autenticacion no disponible");
                        } else {
                            lblError.setText("Usuario o contrasena incorrectos");
                        }
                        txtClave.setText("");
                        txtClave.requestFocus();
                    }
                });
            }, "hilo-login").start();
        };

        btnLogin.addActionListener(doLogin);
        // Enter en cualquier campo tambien dispara el login
        txtUsuario.addActionListener(doLogin);
        txtClave.addActionListener(doLogin);
    }

    // Envia LOGIN|usuario|clave al servidor y devuelve la respuesta
    String intentarLogin(String usuario, String clave) {
        InetSocketAddress addr = resolverHost();
        if (addr == null) return "ERR|DNS_FAILED";

        try (Socket sock = new Socket()) {
            sock.connect(addr, 5000);
            sock.setSoTimeout(8000);   // timeout para la respuesta del login

            OutputStream out = sock.getOutputStream();
            InputStream  in  = sock.getInputStream();

            String msg = "LOGIN|" + usuario + "|" + clave + "\n";
            out.write(msg.getBytes("UTF-8"));
            out.flush();

            return leerLinea(in).trim();

        } catch (SocketTimeoutException e) {
            return "ERR|TIMEOUT";
        } catch (IOException e) {
            return "ERR|CONNECTION_FAILED";
        }
    }

    // ========================================================================
    // DASHBOARD PRINCIPAL (solo se muestra tras login exitoso)
    // ========================================================================
    void buildDashboard() {
        running = true;

        frame = new JFrame("EAFIT IoT Monitor");
        frame.setDefaultCloseOperation(JFrame.DO_NOTHING_ON_CLOSE);
        frame.addWindowListener(new WindowAdapter() {
            public void windowClosing(WindowEvent e) {
                running = false;
                frame.dispose();
            }
        });
        frame.setSize(860, 580);
        frame.setLocationRelativeTo(null);
        frame.getContentPane().setBackground(BG_DARK);
        frame.setLayout(new BorderLayout(0, 8));

        // -- Barra superior --------------------------------------------------
        JPanel topPanel = new JPanel(new BorderLayout());
        topPanel.setBackground(BG_DARK);
        topPanel.setBorder(BorderFactory.createEmptyBorder(12, 16, 4, 16));

        JLabel title = new JLabel("Sistema de Monitoreo IoT - EAFIT");
        title.setFont(new Font("SansSerif", Font.BOLD, 15));
        title.setForeground(FG_TEXT);
        topPanel.add(title, BorderLayout.WEST);

        // Info del usuario en la derecha
        JPanel userInfo = new JPanel(new FlowLayout(FlowLayout.RIGHT, 8, 0));
        userInfo.setBackground(BG_DARK);

        JLabel lblUsuarioInfo = new JLabel(usuarioActual);
        lblUsuarioInfo.setFont(new Font("SansSerif", Font.BOLD, 12));
        lblUsuarioInfo.setForeground(ACCENT);

        JLabel lblRolInfo = new JLabel("[" + rolActual + "]");
        lblRolInfo.setFont(new Font("SansSerif", Font.PLAIN, 11));
        lblRolInfo.setForeground(new Color(0x6c7086));

        JButton btnLogout = new JButton("Cerrar sesion");
        btnLogout.setBackground(BG_SURFACE);
        btnLogout.setForeground(FG_RED);
        btnLogout.setFont(new Font("SansSerif", Font.PLAIN, 11));
        btnLogout.setFocusPainted(false);
        btnLogout.setBorderPainted(false);
        btnLogout.setCursor(Cursor.getPredefinedCursor(Cursor.HAND_CURSOR));
        btnLogout.addActionListener(e -> {
            running = false;
            frame.dispose();
            // Volver a la pantalla de login
            SwingUtilities.invokeLater(() -> new OperatorDashboard(host, port).mostrarLogin());
        });

        userInfo.add(lblUsuarioInfo);
        userInfo.add(lblRolInfo);
        userInfo.add(btnLogout);
        topPanel.add(userInfo, BorderLayout.EAST);

        // Status de conexion
        statusLabel = new JLabel("Conectando...");
        statusLabel.setFont(new Font("SansSerif", Font.PLAIN, 10));
        statusLabel.setForeground(FG_GREEN);
        JPanel statusPanel = new JPanel(new FlowLayout(FlowLayout.LEFT, 0, 0));
        statusPanel.setBackground(BG_DARK);
        statusPanel.add(statusLabel);
        topPanel.add(statusPanel, BorderLayout.SOUTH);

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

        alertsArea = new JTextArea(5, 0);
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

        // Arrancar hilos de datos
        startPolling();
        startAlertListener();
    }

    // -- Helpers thread-safe -------------------------------------------------
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
            String[] partes = entry.split(":", 4);  // limite 4: timestamp HH:mm:ss tiene ":"
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

    // -- Hilo 1: GET_STATE cada 2 segundos -----------------------------------
    void startPolling() {
        Thread t = new Thread(() -> {
            while (running) {
                InetSocketAddress addr = resolverHost();
                if (addr != null) {
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
                    } catch (SocketTimeoutException e) {
                        setStatus("Timeout - servidor no responde", FG_RED);
                    } catch (ConnectException e) {
                        setStatus("Sin conexion al servidor", FG_RED);
                    } catch (Exception e) {
                        setStatus("Error: " + e.getMessage(), FG_RED);
                    }
                } else {
                    setStatus("DNS no resuelto para " + host, FG_RED);
                }
                try { Thread.sleep(POLL_MS); } catch (InterruptedException ignored) {}
            }
        }, "hilo-polling");
        t.setDaemon(true);
        t.start();
    }

    // -- Hilo 2: SUBSCRIBE - alertas push ------------------------------------
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

                    sock.setSoTimeout(5000);
                    String ack = leerLinea(in);
                    if (!ack.contains("OK")) {
                        Thread.sleep(RECONECT_MS);
                        continue;
                    }

                    sock.setSoTimeout(0);
                    while (running) {
                        String linea = leerLinea(in);
                        if (linea.isEmpty()) break;
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

    // -- main ----------------------------------------------------------------
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

        final String fHost = host;
        final int    fPort = port;

        System.out.println("=== EAFIT IoT Monitor ===");
        System.out.println("Host: " + host + "  Puerto: " + port);

        SwingUtilities.invokeLater(() ->
            new OperatorDashboard(fHost, fPort).mostrarLogin()
        );
    }
}